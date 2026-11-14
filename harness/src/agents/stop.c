#include "agents_internal.h"

ah_code agents_stop(ah_engine *e, const char *sender, const char *recipient, ah_error *err) {
    char from[AH_ID_CAP], to[AH_ID_CAP];
    ah_code rc = agents_root(e, sender, from, err);
    if (rc == AH_OK)
        rc = agents_root(e, recipient, to, err);
    if (rc != AH_OK)
        return rc;
    if (strcmp(from, to) || !strcmp(sender, recipient) || !strcmp(to, recipient))
        return ah_fail(err, AH_INVALID, "stop requires another non-root agent in this tree");
    sqlite3_stmt *s = NULL;
    rc = core_prepare(e,
                      "UPDATE agent_nodes SET generation=generation+1,resume_pending=0,"
                      "wake_after=(SELECT COALESCE(MAX(seq),0) FROM agent_messages) "
                      "WHERE session_id=?",
                      &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, recipient, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    bool unknown = false;
    s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "SELECT id FROM operations WHERE session_id=? AND status!='done' "
                          "ORDER BY seq",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, recipient, -1, SQLITE_TRANSIENT);
        int step;
        while (rc == AH_OK && (step = sqlite3_step(s)) == SQLITE_ROW) {
            core_operation op;
            rc =
                core_get_operation(e, recipient, (const char *)sqlite3_column_text(s, 0), &op, err);
            if (rc != AH_OK)
                break;
            bool executing = !strcmp(op.status, "executing");
            if (!strcmp(op.status, "unknown") || (executing && op.effect != AH_EFFECT_READ)) {
                unknown = true;
                if (executing)
                    rc = core_set_operation(e, recipient, op.id, "unknown", err);
            } else if (op.result &&
                       (!strcmp(op.status, "received") || !strcmp(op.status, "received_late") ||
                        !strcmp(op.status, "resolved"))) {
                if (!unknown)
                    rc = core_receipt(e, recipient, &op, op.result, op.is_error, "tool.completed",
                                      err);
            } else if (unknown)
                rc = core_save_result(e, recipient, &op,
                                      "{\"error\":\"agent interrupted before execution\"}", true,
                                      "received", "tool.interrupted", err);
            else
                rc = core_receipt(e, recipient, &op,
                                  "{\"error\":\"agent interrupted before execution\"}", true,
                                  "tool.interrupted", err);
            core_operation_free(&op);
        }
        if (rc == AH_OK && step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "cannot settle interrupted agent operations");
    }
    sqlite3_finalize(s);
    s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "DELETE FROM agent_waits WHERE operation_id IN "
                          "(SELECT id FROM operations WHERE session_id=? AND status='done')",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, recipient, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_state(e, recipient, unknown ? AH_OUTCOME_UNKNOWN : AH_STOPPED, err);
    char *event = core_object_string("sender", sender);
    if (rc == AH_OK && !event)
        rc = ah_fail(err, AH_IO, "out of memory");
    if (rc == AH_OK)
        rc = core_event(e, recipient, NULL, "agent.interrupted", event, err);
    free(event);
    return rc;
}

ah_code ah_agent_interrupt(ah_engine *e, const char *sender, const char *recipient, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!e->agents_enabled)
        return ah_fail(err, AH_INVALID, "agents are not enabled");
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = agents_stop(e, sender, recipient, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    return rc;
}
