#include "core.h"
#include "../agents/agents_internal.h"

typedef struct {
    ah_engine *engine;
    const char *session;
} delta_context;
static void on_delta(void *context, const char *text, size_t length) {
    delta_context *sink = context;
    core_emit_delta(sink->engine, sink->session, text, length);
}
ah_code ah_run(ah_engine *e, const char *sid, ah_error *err) {
    if (e && e->agents_enabled)
        return agents_run(e, sid, err);
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    core_session session;
    rc = core_load(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    if (session.state == AH_COMPLETED || session.state == AH_WAITING_APPROVAL ||
        session.state == AH_WAITING_INPUT) {
        core_session_free(&session);
        return AH_OK;
    }
    if (session.state != AH_READY) {
        ah_state state = session.state;
        core_session_free(&session);
        return ah_fail(err, state == AH_OUTCOME_UNKNOWN ? AH_UNCERTAIN : AH_INVALID,
                       "session must be ready; submit or resume explicitly");
    }
    e->running = true;
    core_mailbox_active(e, sid);
    uint64_t now = ah_now_ms();
    e->deadline = now + e->config.run_timeout_ms;
    if (e->deadline < now)
        e->deadline = UINT64_MAX;
    rc = core_task_allow(e, sid, true, 0, NULL, err);
    if (rc == AH_OK)
        rc = core_task_deadline(e, sid, &e->deadline, err);
    if (rc == AH_OK)
        rc = core_run_transition(e, sid, AH_RUNNING, "run.started", "{}", err);
    session.state = AH_RUNNING;
    while (rc == AH_OK && session.state == AH_RUNNING) {
        if (core_cancelled(e)) {
            rc = ah_fail(err,
                         atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                   : AH_LIMIT,
                         "run cancelled or deadline exceeded");
            break;
        }
        core_operation op;
        ah_code found = core_next_operation(e, sid, &op, err);
        if (found == AH_OK) {
            bool batched = false;
            rc = core_execute_parallel(e, &session, &op, &batched, err);
            if (rc == AH_OK && !batched)
                rc = core_execute_operation(e, &session, &op, err);
            core_operation_free(&op);
            continue;
        }
        if (found != AH_NOT_FOUND) {
            rc = found;
            break;
        }
        rc = core_drain_steering(e, &session, false, err);
        if (rc != AH_OK)
            break;
        if (session.model_calls >= e->config.max_model_calls) {
            rc = ah_fail(err, AH_LIMIT, "model call budget exhausted");
            break;
        }
        rc = core_task_allow(e, sid, true, 1, NULL, err);
        if (rc != AH_OK)
            break;
        char *request = NULL;
        uint64_t assembled = ah_now_us();
        rc = core_request(e, &session, &request, err);
        core_metric(e, sid, NULL, "request", assembled, ah_now_us(), request ? strlen(request) : 0,
                    session.model_calls + 1, rc);
        if (rc != AH_OK)
            break;
        rc = core_model_intent(e, &session, err);
        if (rc != AH_OK) {
            free(request);
            break;
        }
        if (core_cancelled(e)) {
            free(request);
            rc = ah_fail(err, AH_CANCELLED, "cancelled before model call");
            break;
        }
        char *response = NULL;
        delta_context sink = {e, sid};
        uint64_t started = ah_now_us();
        rc = e->config.provider.complete(e->config.provider.context, request, on_delta, &sink,
                                         core_cancelled, e, &response, err);
        core_metric(e, sid, NULL, "model", started, ah_now_us(), response ? strlen(response) : 0,
                    session.model_calls, rc);
        free(request);
        rc = core_model_finish(e, &session, response, rc, err);
        free(response);
    }
    rc = core_run_failure(e, &session, rc, err, NULL);
    core_mailbox_active(e, NULL);
    e->deadline = 0;
    e->running = false;
    core_session_free(&session);
    return rc;
}
