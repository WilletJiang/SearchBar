#include "scheduler.h"

static bool cancelled(void *context) {
    agent_job *job = context;
    return job->cancelled || atomic_load_explicit(&job->engine->cancelled, memory_order_relaxed) ||
           ah_now_ms() >= job->deadline;
}

static void delta(void *context, const char *text, size_t length) {
    agent_job *job = context;
    if (!cancelled(job))
        core_emit_delta(job->engine, job->session.id, text, length);
}

ah_code agents_job_notify(agent_job *job, ah_error *error) {
    core_session *session = &job->session;
    if (session->state == AH_RUNNING || session->state == AH_READY || session->state == AH_IDLE ||
        session->state == AH_WAITING_AGENTS)
        return AH_OK;
    if (job->notified && job->notified_turn == session->turn &&
        job->notified_state == session->state && job->notified_generation == job->generation &&
        job->notified_attempt == session->model_calls && job->notified_tools == session->tool_calls)
        return AH_OK;
    ah_code code = agents_notify(job->engine, session, error);
    if (code == AH_OK) {
        job->notified = true;
        job->notified_turn = session->turn;
        job->notified_state = session->state;
        job->notified_generation = job->generation;
        job->notified_attempt = session->model_calls;
        job->notified_tools = session->tool_calls;
    }
    return code;
}

void agents_model_release(agent_job *job) {
    if (job->handle) {
        ah_async_provider *provider = &job->engine->agents.provider;
        provider->release(provider->context, job->handle);
        job->handle = NULL;
    }
    free(job->request);
    job->request = NULL;
}

ah_code agents_model_start(agent_job *job, ah_error *error) {
    ah_engine *e = job->engine;
    core_session *session = &job->session;
    job->cancelled = false;
    if (session->model_calls >= e->config.max_model_calls)
        return ah_fail(error, AH_LIMIT, "model call budget exhausted");
    ah_code code = core_task_allow(e, session->id, true, 1, NULL, error);
    if (code == AH_OK)
        code = core_task_deadline(e, session->id, &job->deadline, error);
    uint64_t assembled = ah_now_us();
    if (code == AH_OK)
        code = core_request(e, session, &job->request, error);
    core_metric(e, session->id, NULL, "request", assembled, ah_now_us(),
                job->request ? strlen(job->request) : 0, session->model_calls + 1, code);
    if (code == AH_OK)
        code = core_model_intent(e, session, error);
    if (code != AH_OK) {
        agents_model_release(job);
        return code;
    }
    if (cancelled(job)) {
        agents_model_release(job);
        return ah_fail(error,
                       atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                 : AH_LIMIT,
                       "cancelled before model dispatch");
    }
    job->request_generation = job->generation;
    job->request_turn = session->turn;
    job->request_attempt = session->model_calls;
    job->started = ah_now_us();
    ah_async_provider *provider = &e->agents.provider;
    code = provider->start(provider->context, job->request, delta, job, cancelled, job,
                           &job->handle, error);
    if (code == AH_OK && !job->handle)
        code = ah_fail(error, AH_PROTOCOL, "async provider returned no request handle");
    if (code != AH_OK)
        agents_model_release(job);
    return code;
}

/* A rejected or interrupted response can still report incurred usage. Record
 * that receipt against the dispatched attempt, even if the session advanced. */
static ah_code save_usage(agent_job *job, const char *response, ah_error *error) {
    if (!response || strlen(response) > job->engine->config.max_output_bytes)
        return AH_OK;
    yyjson_doc *doc = yyjson_read(response, strlen(response), 0);
    if (!doc)
        return AH_OK;
    core_session attempt = job->session;
    attempt.turn = job->request_turn;
    attempt.model_calls = job->request_attempt;
    uint64_t after = 0;
    ah_code code = core_begin(job->engine, &after, error);
    if (code == AH_OK)
        code = agents_record_usage(job->engine, &attempt, yyjson_doc_get_root(doc), error);
    if (code == AH_OK)
        code = core_commit(job->engine, after, error);
    else
        core_rollback(job->engine);
    yyjson_doc_free(doc);
    return code;
}

ah_code agents_model_finish(agent_job *job, bool *done, ah_error *error) {
    *done = false;
    ah_engine *e = job->engine;
    ah_async_provider *provider = &e->agents.provider;
    bool stale = job->request_generation != job->generation || job->session.state != AH_RUNNING;
    char *response = NULL;
    ah_code code = provider->result(provider->context, job->handle, done, &response, error);
    if (stale) {
        /* A stop may precede this scan within the same owner iteration. */
        job->cancelled = true;
        code = save_usage(job, response, error);
        free(response);
        agents_model_release(job);
        *done = true;
        return code == AH_OK ? agents_job_notify(job, error) : code;
    }
    if (!*done && code == AH_OK) {
        free(response);
        return AH_OK;
    }
    *done = true;
    core_metric(e, job->session.id, NULL, "model", job->started, ah_now_us(),
                response ? strlen(response) : 0, job->session.model_calls, code);
    if (code != AH_OK || cancelled(job)) {
        ah_error usage_error = {0};
        ah_code saved = save_usage(job, response, &usage_error);
        if (saved != AH_OK) {
            code = saved;
            if (error)
                *error = usage_error;
        }
    }
    code = core_model_finish(e, &job->session, response, code, error);
    free(response);
    agents_model_release(job);
    bool changed = false;
    if (code == AH_OK)
        code = agents_drain(e, &job->session, &changed, error);
    if (code == AH_OK)
        code = agents_job_notify(job, error);
    return code;
}
