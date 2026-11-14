#include "agents_internal.h"

static ah_code dispatch(ah_engine *e, const core_session *session, const core_operation *op,
                        yyjson_val *args, char **result, bool *parked, ah_error *err) {
    if (!strcmp(op->name, "spawn"))
        return agents_spawn(e, session, op, args, result, err);
    if (!strcmp(op->name, "join"))
        return agents_join(e, session, op, args, result, parked, err);
    if (!strcmp(op->name, "agents"))
        return agents_list(e, session->id, result, err);
    const char *target = yyjson_get_str(yyjson_obj_get(args, "target"));
    ah_code rc;
    if (!strcmp(op->name, "send")) {
        const char *message = yyjson_get_str(yyjson_obj_get(args, "message"));
        bool wake = yyjson_get_bool(yyjson_obj_get(args, "wake"));
        rc = agents_queue(e, session->id, target, op->id, message, wake, false, err);
    } else
        rc = agents_stop(e, session->id, target, err);
    if (rc == AH_OK) {
        *result = ah_strdup("{\"ok\":true}");
        if (!*result)
            rc = ah_fail(err, AH_IO, "out of memory");
    }
    return rc;
}

ah_code agents_execute(ah_engine *e, core_session *session, core_operation *op, ah_error *err) {
    if (core_cancelled(e))
        return ah_fail(err,
                       atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                 : AH_LIMIT,
                       "coordination interrupted");
    const char *schema = agents_tool_schema(op->name);
    if (!e->agents_enabled || !schema)
        return ah_fail(err, AH_INVALID, "coordination tool unavailable");
    bool waiting = !strcmp(op->status, "agent_wait");
    if (waiting && strcmp(op->name, "join"))
        return ah_fail(err, AH_PROTOCOL, "only join may own a durable agent wait");
    if (!waiting && strcmp(op->status, "pending") && strcmp(op->status, "approved"))
        return ah_fail(err, AH_PROTOCOL, "unexpected coordination operation status");
    if (!waiting && session->tool_calls >= e->config.max_tool_calls)
        return ah_fail(err, AH_LIMIT, "tool call budget exhausted");
    ah_code rc = waiting ? AH_OK : core_task_allow(e, session->id, false, 1, NULL, err);
    if (rc != AH_OK)
        return rc;
    yyjson_doc *parsed = NULL;
    ah_error validation = {0};
    ah_code valid = ah_schema_parse(schema, op->arguments, &parsed, &validation);
    if (valid == AH_OK &&
        (!op->schema || strcmp(op->schema, schema) || op->effect != AH_EFFECT_READ || op->approval))
        valid = ah_fail(&validation, AH_INVALID, "coordination policy changed since intent");
    if (valid == AH_OK)
        valid = ah_json_check(yyjson_doc_get_root(parsed), &validation);
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK && !waiting)
        rc = core_prepare(e, "UPDATE sessions SET tool_calls=tool_calls+1 WHERE id=?", &s, err);
    if (rc == AH_OK && !waiting) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK && !waiting)
        rc = core_task_charge(e, session->id, false, 1, err);
    char *result = NULL;
    bool parked = false, is_error = valid != AH_OK;
    if (rc == AH_OK && valid != AH_OK) {
        result = core_object_string("error", validation.message);
        if (!result)
            rc = ah_fail(err, AH_IO, "out of memory");
    } else if (rc == AH_OK) {
        rc = core_sql(e, "SAVEPOINT agent_action", err);
        if (rc == AH_OK)
            rc = dispatch(e, session, op, yyjson_doc_get_root(parsed), &result, &parked, err);
        if (rc == AH_INVALID || rc == AH_NOT_FOUND || rc == AH_LIMIT) {
            free(result);
            result = core_object_string("error", err && err->message[0] ? err->message
                                                                        : "agent action rejected");
            rc = core_sql(e, "ROLLBACK TO agent_action; RELEASE agent_action", err);
            is_error = true;
            parked = false;
            if (rc == AH_OK && !result)
                rc = ah_fail(err, AH_IO, "out of memory");
        } else if (rc == AH_OK)
            rc = core_sql(e, "RELEASE agent_action", err);
    }
    if (rc == AH_OK && !parked)
        rc = core_receipt(e, session->id, op, result, is_error, "tool.completed", err);
    if (rc == AH_OK && waiting && !parked)
        rc = core_state(e, session->id, AH_READY, err);
    if (rc == AH_OK) {
        rc = core_commit(e, after, err);
        if (rc != AH_OK)
            session->state = AH_OUTCOME_UNKNOWN;
    } else
        core_rollback(e);
    free(result);
    yyjson_doc_free(parsed);
    if (rc == AH_OK) {
        if (!waiting)
            session->tool_calls++;
        if (parked)
            session->state = AH_WAITING_AGENTS;
        else if (waiting)
            session->state = AH_READY;
        ah_error_clear(err);
    }
    return rc;
}
