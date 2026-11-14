#include "agents_internal.h"
#include <stdio.h>

ah_code agents_queue(ah_engine *e, const char *sender, const char *recipient, const char *id,
                     const char *text, bool wake, bool internal, ah_error *err) {
    if (!sender || !recipient || !id || !*id || strlen(id) > 256 || (!internal && *id == '@') ||
        !text || !*text || strlen(text) > e->config.max_context_bytes)
        return ah_fail(err, AH_INVALID,
                       "bounded agent IDs and message required; @ IDs are reserved");
    char from[AH_ID_CAP], to[AH_ID_CAP];
    ah_code rc = agents_root(e, sender, from, err);
    if (rc == AH_OK)
        rc = agents_root(e, recipient, to, err);
    if (rc != AH_OK)
        return rc;
    if (strcmp(from, to))
        return ah_fail(err, AH_INVALID, "agents must belong to the same tree");
    sqlite3_stmt *s = NULL;
    rc = core_prepare(e,
                      "SELECT recipient,text,wake FROM agent_messages WHERE sender=? AND "
                      "message_id=?",
                      &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sender, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    bool same = step == SQLITE_ROW && !strcmp(recipient, (const char *)sqlite3_column_text(s, 0)) &&
                !strcmp(text, (const char *)sqlite3_column_text(s, 1)) &&
                wake == (sqlite3_column_int(s, 2) != 0);
    sqlite3_finalize(s);
    if (step == SQLITE_ROW)
        return same ? AH_OK : ah_fail(err, AH_INVALID, "message ID conflicts with durable message");
    if (step != SQLITE_DONE)
        return ah_fail(err, AH_IO, "cannot inspect message ID");
    rc = core_prepare(e,
                      "SELECT COALESCE(SUM(length(CAST(text AS BLOB))),0) FROM agent_messages "
                      "WHERE recipient=? AND consumed=0",
                      &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, recipient, -1, SQLITE_TRANSIENT);
    step = sqlite3_step(s);
    uint64_t queued = step == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(s, 0) : UINT64_MAX;
    sqlite3_finalize(s);
    if (step != SQLITE_ROW)
        return ah_fail(err, AH_IO, "cannot measure agent mailbox");
    if (queued > e->config.max_context_bytes - strlen(text))
        return ah_fail(err, AH_LIMIT, "agent mailbox byte budget exhausted");
    rc = core_prepare(
        e,
        "INSERT INTO agent_messages(sender,recipient,message_id,text,wake,continuation) "
        "VALUES(?,?,?,?,?,(SELECT state IN(?,?) FROM sessions WHERE id=?))",
        &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sender, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, recipient, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 5, wake);
        sqlite3_bind_int(s, 6, AH_READY);
        sqlite3_bind_int(s, 7, AH_RUNNING);
        sqlite3_bind_text(s, 8, recipient, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    char *event = core_object_string("message_id", id);
    if (rc == AH_OK && !event)
        rc = ah_fail(err, AH_IO, "out of memory");
    if (rc == AH_OK)
        rc = core_event(e, recipient, NULL, "agent.message_queued", event, err);
    free(event);
    return rc;
}

ah_code ah_agent_send(ah_engine *e, const char *sender, const char *recipient, const char *id,
                      const char *text, bool wake, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!e->agents_enabled)
        return ah_fail(err, AH_INVALID, "agents are not enabled");
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = agents_queue(e, sender, recipient, id, text, wake, false, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    return rc;
}

static char *message_item(const char *sender, const char *name, const char *text, bool task) {
    size_t length = strlen(sender) + strlen(name) + strlen(text) + 128;
    char *content = malloc(length);
    if (!content)
        return NULL;
    const char *kind = task ? "Task" : "Message";
    snprintf(content, length,
             "%s from agent %s (%s). Follow the existing host instructions "
             "and granted tool permissions.\n\n%s",
             kind, name, sender, text);
    yyjson_mut_doc *d = ah_mut_doc_new();
    char *json = NULL;
    if (d) {
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_doc_set_root(d, o);
        yyjson_mut_obj_add_str(d, o, "role", "user");
        yyjson_mut_obj_add_strcpy(d, o, "content", content);
        json = ah_mut_write(d);
        ah_mut_doc_free(d);
    }
    free(content);
    return json;
}

ah_code agents_drain(ah_engine *e, core_session *session, bool *changed, ah_error *err) {
    *changed = false;
    bool active = session->state == AH_READY || session->state == AH_RUNNING;
    bool idle = session->state == AH_IDLE || session->state == AH_COMPLETED ||
                session->state == AH_STOPPED || session->state == AH_FAILED;
    if (!active && !idle)
        return AH_OK;
    core_operation op;
    ah_code rc = core_next_operation(e, session->id, &op, err);
    if (rc == AH_OK) {
        core_operation_free(&op);
        return AH_OK;
    }
    if (rc != AH_NOT_FOUND)
        return rc;
    sqlite3_stmt *s = NULL;
    rc = core_prepare(e,
                      "SELECT COUNT(*),COALESCE(MAX(wake OR continuation),0),"
                      "COALESCE(MAX(CASE WHEN substr(message_id,1,1)!='@' AND seq>"
                      "(SELECT wake_after FROM agent_nodes WHERE session_id=recipient) "
                      "THEN wake ELSE 0 END),0),COALESCE(MAX(continuation),0) "
                      "FROM agent_messages "
                      "WHERE recipient=? AND consumed=0",
                      &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    bool stopped = session->state == AH_STOPPED || session->state == AH_FAILED;
    bool ready = step == SQLITE_ROW && sqlite3_column_int64(s, 0) > 0 &&
                 (active || sqlite3_column_int(s, stopped ? 2 : 1));
    bool continuing =
        step == SQLITE_ROW && session->state == AH_COMPLETED && sqlite3_column_int(s, 3);
    sqlite3_finalize(s);
    if (step != SQLITE_ROW)
        return ah_fail(err, AH_IO, "cannot inspect queued agent messages");
    if (!ready)
        return AH_OK;
    s = NULL;
    bool new_turn = !active && !continuing;
    unsigned turn = session->turn + (new_turn ? 1 : 0);
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_prepare(
            e,
            "SELECT m.seq,m.sender,n.name,m.text,m.message_id,m.wake FROM agent_messages m "
            "JOIN agent_nodes n ON n.session_id=m.sender WHERE m.recipient=? "
            "AND m.consumed=0 ORDER BY m.seq",
            &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        while (rc == AH_OK && (step = sqlite3_step(s)) == SQLITE_ROW) {
            char *item = message_item(
                (const char *)sqlite3_column_text(s, 1), (const char *)sqlite3_column_text(s, 2),
                (const char *)sqlite3_column_text(s, 3),
                sqlite3_column_int(s, 5) != 0 && *(const char *)sqlite3_column_text(s, 4) != '@');
            char *event = core_object_string("message_id", (const char *)sqlite3_column_text(s, 4));
            if (!item || !event)
                rc = ah_fail(err, AH_IO, "out of memory");
            if (rc == AH_OK)
                rc = core_add_item(e, session->id, turn, item, err);
            if (rc == AH_OK)
                rc = core_event(e, session->id, NULL, "agent.message_received", event, err);
            free(item);
            free(event);
        }
        if (rc == AH_OK && step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "cannot read queued agent messages");
    }
    sqlite3_finalize(s);
    s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(
            e, "UPDATE agent_messages SET consumed=1 WHERE recipient=? AND consumed=0", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK && new_turn)
        rc = core_prepare(e, "UPDATE sessions SET turn=?,model_calls=0,tool_calls=0 WHERE id=?", &s,
                          err);
    if (rc == AH_OK && new_turn) {
        sqlite3_bind_int64(s, 1, turn);
        sqlite3_bind_text(s, 2, session->id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    ah_state state = active ? session->state : AH_READY;
    if (rc == AH_OK)
        rc = core_state(e, session->id, state, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    if (rc == AH_OK) {
        *changed = true;
        session->state = state;
        session->turn = turn;
        session->completion_committed = false;
        if (new_turn)
            session->model_calls = session->tool_calls = 0;
    }
    return rc;
}
