#include "agents_internal.h"

static bool attention(ah_state state) {
    return state == AH_FAILED || state == AH_STOPPED || state == AH_WAITING_INPUT ||
           state == AH_WAITING_APPROVAL || state == AH_OUTCOME_UNKNOWN;
}

ah_code agents_join(ah_engine *e, const core_session *session, const core_operation *op,
                    yyjson_val *args, char **result, bool *parked, ah_error *err) {
    yyjson_val *ids = yyjson_obj_get(args, "ids");
    size_t count = yyjson_arr_size(ids);
    if (!yyjson_is_arr(ids) || !count || count > 32)
        return ah_fail(err, AH_INVALID, "join requires 1 to 32 descendant IDs");
    yyjson_val *timeout = yyjson_obj_get(args, "timeout_ms");
    double milliseconds = timeout ? yyjson_get_num(timeout) : 60000;
    if (timeout && (!yyjson_is_num(timeout) || milliseconds < 0 || milliseconds > 3600000 ||
                    milliseconds != (unsigned)milliseconds))
        return ah_fail(err, AH_INVALID, "join timeout must be 0 to 3600000 milliseconds");
    uint64_t deadline = core_wall_ms() + (unsigned)milliseconds;
    sqlite3_stmt *s = NULL;
    ah_code rc = AH_OK;
    if (!strcmp(op->status, "agent_wait")) {
        rc = core_prepare(e, "SELECT deadline_ms FROM agent_waits WHERE operation_id=?", &s, err);
        if (rc != AH_OK)
            return rc;
        sqlite3_bind_text(s, 1, op->id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) != SQLITE_ROW)
            rc = ah_fail(err, AH_PROTOCOL, "durable join deadline unavailable");
        else
            deadline = (uint64_t)sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        if (rc != AH_OK)
            return rc;
    }
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return ah_fail(err, AH_IO, "out of memory");
    yyjson_mut_val *o = yyjson_mut_obj(d), *list = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_val(d, o, "agents", list);
    const char *mode = yyjson_get_str(yyjson_obj_get(args, "mode"));
    bool any_mode = mode && !strcmp(mode, "any");
    bool any = false, all = true, needs_attention = false;
    for (size_t i = 0; i < count && rc == AH_OK; i++) {
        const char *id = yyjson_get_str(yyjson_arr_get(ids, i));
        if (!id || !*id) {
            rc = ah_fail(err, AH_INVALID, "join IDs must be nonempty strings");
            break;
        }
        for (size_t j = 0; j < i; j++)
            if (!strcmp(id, yyjson_get_str(yyjson_arr_get(ids, j))))
                rc = ah_fail(err, AH_INVALID, "join IDs must be distinct");
        if (rc != AH_OK)
            break;
        rc = core_prepare(
            e,
            "WITH RECURSIVE ancestors(id,parent) AS (SELECT session_id,parent_id FROM "
            "agent_nodes WHERE session_id=?1 UNION ALL SELECT n.session_id,n.parent_id "
            "FROM agent_nodes n JOIN ancestors a ON n.session_id=a.parent) "
            "SELECT state,turn FROM sessions WHERE id=?1 AND EXISTS "
            "(SELECT 1 FROM ancestors WHERE parent=?2)",
            &s, err);
        if (rc != AH_OK)
            break;
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, session->id, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(s);
        if (step == SQLITE_ROW) {
            ah_state state = sqlite3_column_int(s, 0);
            any |= state == AH_COMPLETED;
            all &= state == AH_COMPLETED;
            needs_attention |= attention(state);
            yyjson_mut_val *item = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, item, "id", id);
            yyjson_mut_obj_add_str(d, item, "state", ah_state_name(state));
            yyjson_mut_obj_add_uint(d, item, "turn", (uint64_t)sqlite3_column_int64(s, 1));
            yyjson_mut_arr_append(list, item);
        } else
            rc = ah_fail(err, step == SQLITE_DONE ? AH_INVALID : AH_IO,
                         "join target must be a descendant of the calling agent");
        sqlite3_finalize(s);
    }
    bool interrupted = false;
    pthread_mutex_lock(&e->mailbox_lock);
    for (core_steering *message = e->mailbox_head; message; message = message->next)
        if (!strcmp(message->session, session->id)) {
            interrupted = true;
            break;
        }
    pthread_mutex_unlock(&e->mailbox_lock);
    bool ready = needs_attention || (any_mode ? any : all);
    *parked = rc == AH_OK && !ready && !interrupted && core_wall_ms() < deadline;
    if (rc == AH_OK && *parked && strcmp(op->status, "agent_wait")) {
        rc = core_prepare(
            e, "INSERT OR IGNORE INTO agent_waits(operation_id,deadline_ms) VALUES(?,?)", &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, op->id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)deadline);
            rc = core_done(e, s, err);
        }
        if (rc == AH_OK && strcmp(op->status, "agent_wait"))
            rc = core_set_operation(e, session->id, op->id, "agent_wait", err);
        if (rc == AH_OK && strcmp(op->status, "agent_wait"))
            rc = core_event(e, session->id, op->id, "agent.waiting", op->arguments, err);
    } else if (rc == AH_OK && !*parked) {
        yyjson_mut_obj_add_bool(d, o, "timed_out", !ready && !interrupted);
        yyjson_mut_obj_add_bool(d, o, "interrupted", interrupted);
        *result = ah_mut_write(d);
        if (!*result)
            rc = ah_fail(err, AH_IO, "out of memory");
        if (rc == AH_OK)
            rc = core_prepare(e, "DELETE FROM agent_waits WHERE operation_id=?", &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, op->id, -1, SQLITE_TRANSIENT);
            rc = core_done(e, s, err);
        }
    }
    if (rc == AH_OK && *parked && session->state != AH_WAITING_AGENTS)
        rc = core_state(e, session->id, AH_WAITING_AGENTS, err);
    ah_mut_doc_free(d);
    return rc;
}
