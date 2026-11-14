#include "agents_internal.h"
#include <stdio.h>

static char *completion_result(ah_engine *e, const core_session *session, const char *name,
                               sqlite3_int64 generation, ah_error *err) {
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d) {
        ah_fail(err, AH_IO, "out of memory");
        return NULL;
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_strcpy(d, o, "agent_id", session->id);
    yyjson_mut_obj_add_strcpy(d, o, "name", name);
    yyjson_mut_obj_add_str(d, o, "state", ah_state_name(session->state));
    yyjson_mut_obj_add_uint(d, o, "turn", session->turn);
    yyjson_mut_obj_add_uint(d, o, "generation", (uint64_t)generation);
    yyjson_mut_obj_add_uint(d, o, "model_calls", session->model_calls);
    yyjson_mut_obj_add_uint(d, o, "tool_calls", session->tool_calls);
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT json FROM items WHERE session_id=? AND turn=? "
                              "ORDER BY seq DESC",
                              &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, session->turn);
        int step;
        while ((step = sqlite3_step(s)) == SQLITE_ROW) {
            const char *json = (const char *)sqlite3_column_text(s, 0);
            yyjson_doc *parsed = yyjson_read(json, (size_t)sqlite3_column_bytes(s, 0), 0);
            yyjson_val *item = parsed ? yyjson_doc_get_root(parsed) : NULL;
            const char *role = yyjson_get_str(yyjson_obj_get(item, "role"));
            bool assistant = role && !strcmp(role, "assistant");
            if (assistant)
                yyjson_mut_obj_add_val(d, o, "output", yyjson_val_mut_copy(d, item));
            yyjson_doc_free(parsed);
            if (assistant)
                break;
        }
        if (step != SQLITE_ROW && step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "cannot read agent completion");
    }
    sqlite3_finalize(s);
    s = NULL;
    if (rc == AH_OK && (session->state == AH_FAILED || session->state == AH_STOPPED)) {
        rc = core_prepare(
            e,
            "SELECT type,json FROM events WHERE session_id=? AND type IN "
            "('run.stopped','agent.interrupted','agent.suspended','session.recovered') "
            "ORDER BY seq DESC LIMIT 1",
            &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
            int step = sqlite3_step(s);
            if (step == SQLITE_ROW) {
                yyjson_mut_obj_add_strcpy(d, o, "reason_event",
                                          (const char *)sqlite3_column_text(s, 0));
                yyjson_doc *reason = yyjson_read((const char *)sqlite3_column_text(s, 1),
                                                 (size_t)sqlite3_column_bytes(s, 1), 0);
                if (reason)
                    yyjson_mut_obj_add_val(d, o, "reason",
                                           yyjson_val_mut_copy(d, yyjson_doc_get_root(reason)));
                yyjson_doc_free(reason);
            } else if (step != SQLITE_DONE)
                rc = ah_fail(err, AH_IO, "cannot read agent stop reason");
        }
        sqlite3_finalize(s);
    }
    if (rc == AH_OK) {
        core_operation op;
        rc = core_next_operation(e, session->id, &op, err);
        if (rc == AH_OK) {
            yyjson_mut_val *pending = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, pending, "operation_id", op.id);
            yyjson_mut_obj_add_strcpy(d, pending, "name", op.name);
            yyjson_mut_obj_add_strcpy(d, pending, "status", op.status);
            yyjson_doc *args = yyjson_read(op.arguments, strlen(op.arguments), 0);
            if (args)
                yyjson_mut_obj_add_val(d, pending, "arguments",
                                       yyjson_val_mut_copy(d, yyjson_doc_get_root(args)));
            yyjson_doc_free(args);
            yyjson_mut_obj_add_val(d, o, "pending", pending);
            core_operation_free(&op);
        } else if (rc == AH_NOT_FOUND) {
            ah_error_clear(err);
            rc = AH_OK;
        }
    }
    char *text = rc == AH_OK ? ah_mut_write(d) : NULL;
    if (rc == AH_OK && !text)
        ah_fail(err, AH_IO, "out of memory");
    ah_mut_doc_free(d);
    return text;
}

static char *notification(ah_engine *e, const core_session *session, const char *operation,
                          const char *result) {
    size_t length = strlen(result), limit = e->config.max_context_bytes / 4;
    if (limit > 4096)
        limit = 4096;
    size_t bytes = length > limit ? limit : length;
    while (bytes < length && bytes && ((unsigned char)result[bytes] & 0xc0) == 0x80)
        bytes--;
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return NULL;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_strcpy(d, o, "agent_id", session->id);
    yyjson_mut_obj_add_str(d, o, "state", ah_state_name(session->state));
    yyjson_mut_obj_add_uint(d, o, "turn", session->turn);
    yyjson_mut_obj_add_strcpy(d, o, "operation_id", operation);
    yyjson_mut_obj_add_str(d, o, "retrieval_tool", "read_result");
    yyjson_doc *parsed = yyjson_read(result, length, 0);
    yyjson_val *pending = yyjson_obj_get(parsed ? yyjson_doc_get_root(parsed) : NULL, "pending");
    const char *pending_id = yyjson_get_str(yyjson_obj_get(pending, "operation_id"));
    const char *pending_tool = yyjson_get_str(yyjson_obj_get(pending, "name"));
    if (pending_id)
        yyjson_mut_obj_add_strcpy(d, o, "pending_operation_id", pending_id);
    if (pending_tool)
        yyjson_mut_obj_add_strcpy(d, o, "pending_tool", pending_tool);
    const char *failure = yyjson_get_str(yyjson_obj_get(
        yyjson_obj_get(parsed ? yyjson_doc_get_root(parsed) : NULL, "reason"), "error"));
    if (failure)
        yyjson_mut_obj_add_strcpy(d, o, "error", failure);
    const char *question =
        yyjson_get_str(yyjson_obj_get(yyjson_obj_get(pending, "arguments"), "question"));
    if (question) {
        size_t question_length = strlen(question),
               shown = question_length > 512 ? 512 : question_length;
        while (shown < question_length && shown && ((unsigned char)question[shown] & 0xc0) == 0x80)
            shown--;
        yyjson_mut_obj_add_strncpy(d, o, "question", question, shown);
    }
    yyjson_doc_free(parsed);
    yyjson_mut_obj_add_strncpy(d, o, "result_preview", result, bytes);
    yyjson_mut_obj_add_bool(d, o, "result_truncated", bytes < length);
    char *text = ah_mut_write(d);
    ah_mut_doc_free(d);
    return text;
}

ah_code agents_notify(ah_engine *e, const core_session *session, ah_error *err) {
    ah_state state = session->state;
    if (state != AH_COMPLETED && state != AH_FAILED && state != AH_STOPPED &&
        state != AH_WAITING_INPUT && state != AH_WAITING_APPROVAL && state != AH_OUTCOME_UNKNOWN)
        return AH_OK;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT parent_id,name,generation FROM agent_nodes "
                              "WHERE session_id=? AND parent_id IS NOT NULL",
                              &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(s);
        return step == SQLITE_DONE ? AH_OK : ah_fail(err, AH_IO, "cannot read agent parent");
    }
    char *parent = ah_strdup((const char *)sqlite3_column_text(s, 0));
    char *name = ah_strdup((const char *)sqlite3_column_text(s, 1));
    sqlite3_int64 generation = sqlite3_column_int64(s, 2);
    sqlite3_finalize(s);
    if (!parent || !name) {
        free(parent);
        free(name);
        return ah_fail(err, AH_IO, "out of memory");
    }
    char id[96], operation[AH_ID_CAP];
    snprintf(id, sizeof(id), "@terminal:%u:%d:%lld:%u:%u", session->turn, state,
             (long long)generation, session->model_calls, session->tool_calls);
    s = NULL;
    rc = core_prepare(e, "SELECT 1 FROM agent_messages WHERE sender=? AND message_id=?", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        step = sqlite3_step(s);
        if (step != SQLITE_ROW && step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "cannot inspect agent notification");
    }
    sqlite3_finalize(s);
    if (rc != AH_OK || step == SQLITE_ROW) {
        free(parent);
        free(name);
        return rc;
    }
    char *result = completion_result(e, session, name, generation, err);
    free(name);
    if (!result)
        rc = err && err->code ? err->code : AH_IO;
    if (rc == AH_OK)
        rc = core_id(operation, err);
    char *text = rc == AH_OK ? notification(e, session, operation, result) : NULL;
    if (rc == AH_OK && !text)
        rc = ah_fail(err, AH_IO, "out of memory");
    uint64_t after = 0;
    if (rc == AH_OK)
        rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "INSERT INTO operations(id,session_id,call_id,name,arguments,effect,"
                          "approval,status,result,is_error) VALUES(?,?,?,'agent_result','{}',"
                          "0,0,'done',?,0)",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, session->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, result, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_prepare(e, "INSERT INTO result_refs(session_id,operation_id) VALUES(?,?),(?,?)",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, operation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, parent, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, operation, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = agents_queue(e, session->id, parent, id, text, true, true, err);
    if (rc == AH_OK)
        rc = core_event(e, session->id, operation, "agent.result", text, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    free(parent);
    free(result);
    free(text);
    return rc;
}
