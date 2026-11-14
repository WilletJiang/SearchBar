#include "core.h"
#include "../agents/agents_internal.h"

void core_emit_delta(ah_engine *e, const char *sid, const char *text, size_t length) {
    if (!e->config.observer || !text || core_cancelled(e))
        return;
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_strncpy(d, o, "text", text, length);
    char *json = ah_mut_write(d);
    if (json) {
        ah_event event = {0, sid, NULL, "model.delta", json};
        e->config.observer(e->config.observer_context, &event);
        free(json);
    }
    ah_mut_doc_free(d);
}
ah_code core_run_transition(ah_engine *e, const char *sid, ah_state state, const char *event,
                            const char *json, ah_error *err) {
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_state(e, sid, state, err);
    if (rc == AH_OK && e->agents_enabled && (state == AH_STOPPED || state == AH_FAILED)) {
        sqlite3_stmt *stmt = NULL;
        rc = core_prepare(e,
                          "UPDATE agent_nodes SET wake_after=(SELECT COALESCE(MAX(seq),0) "
                          "FROM agent_messages WHERE recipient=?) WHERE session_id=?",
                          &stmt, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, sid, -1, SQLITE_TRANSIENT);
            rc = core_done(e, stmt, err);
        }
    }
    if (rc == AH_OK && event)
        rc = core_event(e, sid, NULL, event, json, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    return rc;
}
ah_code core_model_intent(ah_engine *e, core_session *session, ah_error *err) {
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e, "UPDATE sessions SET model_calls=model_calls+1 WHERE id=?", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_task_charge(e, session->id, true, 1, err);
    if (rc == AH_OK && e->agents_enabled)
        rc = agents_usage_intent(e, session, err);
    if (rc == AH_OK)
        rc = core_event(e, session->id, NULL, "model.started", "{}", err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    if (rc == AH_OK)
        session->model_calls++;
    return rc;
}
ah_code core_model_finish(ah_engine *e, core_session *session, const char *response, ah_code rc,
                          ah_error *err) {
    if (core_cancelled(e)) {
        uint64_t after = 0;
        ah_code saved = core_begin(e, &after, err);
        const char *late =
            response && strlen(response) <= e->config.max_output_bytes && ah_json_valid(response)
                ? response
                : "{\"discarded\":true}";
        if (saved == AH_OK)
            saved = core_event(e, session->id, NULL, "model.late_result", late, err);
        if (saved == AH_OK)
            saved = core_commit(e, after, err);
        else
            core_rollback(e);
        rc = saved == AH_OK
                 ? ah_fail(err,
                           atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                     : AH_LIMIT,
                           "model returned after cancellation or deadline")
                 : saved;
        return rc;
    }
    if (rc == AH_OK)
        rc = core_accept_response(e, session, response, err);
    if (rc == AH_OK && session->state == AH_COMPLETED) {
        rc = core_drain_steering(e, session, true, err);
        if (rc == AH_OK && session->state == AH_COMPLETED && !session->completion_committed)
            rc = core_run_transition(e, session->id, AH_COMPLETED, NULL, NULL, err);
    }
    return rc;
}

ah_code core_run_failure(ah_engine *e, core_session *session, ah_code rc, ah_error *err,
                         bool *settled) {
    if (settled)
        *settled = rc == AH_OK || session->state == AH_OUTCOME_UNKNOWN;
    if (rc != AH_OK && session->state != AH_OUTCOME_UNKNOWN) {
        ah_error original = {0};
        if (err)
            original = *err;
        ah_state failed = (rc == AH_CANCELLED || rc == AH_LIMIT) ? AH_STOPPED : AH_FAILED;
        char *json =
            core_object_string("error", err && err->message[0] ? err->message : ah_code_name(rc));
        ah_error save_error = {0};
        ah_code saved = core_run_transition(e, session->id, failed, "run.stopped",
                                            json ? json : "{}", &save_error);
        free(json);
        if (saved != AH_OK) {
            rc = saved;
            if (err)
                *err = save_error;
        } else {
            session->state = failed;
            if (settled)
                *settled = true;
            if (err)
                *err = original;
        }
    }
    return rc;
}
