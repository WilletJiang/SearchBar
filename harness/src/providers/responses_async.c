#include "responses_internal.h"

static responses_request *find_request(ah_responses *provider, void *handle) {
    for (responses_request *t = provider->requests; t; t = t->next)
        if (t == handle)
            return t;
    return NULL;
}

static ah_code start(void *context, const char *json, ah_delta_sink delta, void *delta_context,
                     ah_cancel_check cancelled, void *cancel_context, void **handle,
                     ah_error *error) {
    ah_responses *provider = context;
    ah_error_clear(error);
    if (handle)
        *handle = NULL;
    if (!provider || !json || !handle)
        return ah_fail(error, AH_INVALID, "invalid provider request");
    if (provider->dispatching)
        return ah_fail(error, AH_BUSY, "provider callback is running");
    if (!provider->token)
        return ah_fail(error, AH_AUTH, "provider requires a bearer token");
    responses_request *t = calloc(1, sizeof(*t));
    if (!t)
        return ah_fail(error, AH_IO, "provider allocation failed");
    uint64_t now = ah_now_ms();
    *t = (responses_request){.provider = provider,
                             .json = json,
                             .request_bytes = strlen(json),
                             .delta = delta,
                             .delta_context = delta_context,
                             .cancelled = cancelled,
                             .cancel_context = cancel_context,
                             .deadline = provider->config.timeout_ms > UINT64_MAX - now
                                             ? UINT64_MAX
                                             : now + provider->config.timeout_ms,
                             .limit = provider->config.max_response_bytes,
                             .measure = provider->config.observe != NULL};
    ah_code code = responses_request_setup(t, error);
    if (!code) {
        code = responses_attempt_start(t);
        if (code && error)
            *error = t->error;
    }
    if (code) {
        responses_request_free(t);
        return code;
    }
    t->next = provider->requests;
    provider->requests = t;
    *handle = t;
    return AH_OK;
}

/* Retry waits are request state, never sleeps inside one request's completion. */
static bool advance_timers(ah_responses *provider) {
    bool completed = false;
    for (responses_request *t = provider->requests; t; t = t->next) {
        if (t->done)
            continue;
        if (responses_check_stop(t)) {
            if (t->active)
                responses_attempt_finish(t, CURLE_ABORTED_BY_CALLBACK);
            else
                t->done = true;
        } else if (!t->active && ah_now_ms() >= t->retry_at) {
            if (responses_attempt_start(t))
                t->done = true;
        }
        completed |= t->done;
    }
    return completed;
}

static ah_code drive(ah_responses *provider, bool *completed, ah_error *error) {
    *completed = advance_timers(provider);
    for (responses_request *t = provider->requests; t; t = t->next)
        if (t->active)
            t->measurement.curl_started = true;
    int running = 0;
    if (curl_multi_perform(provider->multi, &running) != CURLM_OK)
        return ah_fail(error, AH_NETWORK, "provider multi transfer failed");
    int pending;
    CURLMsg *message;
    while ((message = curl_multi_info_read(provider->multi, &pending))) {
        if (message->msg != CURLMSG_DONE)
            continue;
        responses_request *t = NULL;
        curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &t);
        if (t && t->active) {
            responses_attempt_finish(t, message->data.result);
            *completed |= t->done;
        }
    }
    *completed |= advance_timers(provider);
    return AH_OK;
}

static ah_code poll_requests(void *context, unsigned timeout_ms, ah_error *error) {
    ah_responses *provider = context;
    ah_error_clear(error);
    if (!provider)
        return ah_fail(error, AH_INVALID, "invalid provider");
    if (provider->dispatching)
        return ah_fail(error, AH_BUSY, "provider callback is running");
    provider->dispatching = true;
    uint64_t started = ah_now_ms();
    bool completed;
    ah_code code = drive(provider, &completed, error);
    uint64_t elapsed = ah_now_ms() - started;
    if (code || completed || elapsed >= timeout_ms)
        goto done;
    unsigned wait_ms = timeout_ms - (unsigned)elapsed;
    if (wait_ms > 20)
        wait_ms = 20; /* Bound cancellation latency even without socket activity. */
    uint64_t now = ah_now_ms();
    for (responses_request *t = provider->requests; t; t = t->next) {
        if (t->done)
            continue;
        uint64_t next = t->deadline;
        if (!t->active && t->retry_at < next)
            next = t->retry_at;
        uint64_t remaining = next > now ? next - now : 0;
        if (remaining < wait_ms)
            wait_ms = (unsigned)remaining;
    }
    if (curl_multi_poll(provider->multi, NULL, 0, (int)wait_ms, NULL) != CURLM_OK)
        code = ah_fail(error, AH_NETWORK, "provider multi poll failed");
    else
        code = drive(provider, &completed, error);
done:
    if (code) {
        /* A pool-level failure must settle every handle before returning it. */
        for (responses_request *t = provider->requests; t; t = t->next) {
            if (t->done)
                continue;
            ah_fail(&t->error, code, "provider multi transfer failed");
            if (t->active)
                responses_attempt_finish(t, CURLE_FAILED_INIT);
            else
                t->done = true;
        }
    }
    provider->dispatching = false;
    return code;
}

static ah_code result(void *context, void *handle, bool *done, char **response,
                      ah_error *error) {
    ah_responses *provider = context;
    ah_error_clear(error);
    if (done)
        *done = false;
    if (response)
        *response = NULL;
    if (!provider || !done || !response)
        return ah_fail(error, AH_INVALID, "invalid provider result");
    if (provider->dispatching)
        return ah_fail(error, AH_BUSY, "provider callback is running");
    responses_request *t = find_request(provider, handle);
    if (!t)
        return ah_fail(error, AH_INVALID, "invalid provider handle");
    *done = t->done;
    if (!t->done)
        return AH_OK;
    if (!t->error.code && !t->response)
        return ah_fail(error, AH_INVALID, "provider result already transferred");
    if (error)
        *error = t->error;
    *response = t->response;
    t->response = NULL;
    return t->error.code;
}

static void release(void *context, void *handle) {
    ah_responses *provider = context;
    if (!provider || provider->dispatching)
        return;
    responses_request **link = &provider->requests;
    while (*link && *link != handle)
        link = &(*link)->next;
    if (!*link)
        return;
    responses_request *t = *link;
    if (t->active) {
        provider->dispatching = true;
        ah_fail(&t->error, AH_CANCELLED, "provider request released");
        responses_attempt_finish(t, CURLE_ABORTED_BY_CALLBACK);
        provider->dispatching = false;
    }
    *link = t->next;
    responses_request_free(t);
}

ah_async_provider ah_responses_async_provider(ah_responses *provider) {
    return (ah_async_provider){.context = provider,
                               .start = start,
                               .poll = poll_requests,
                               .result = result,
                               .release = release};
}

ah_code responses_complete(void *context, const char *json, ah_delta_sink delta,
                           void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                           char **response, ah_error *error) {
    ah_responses *provider = context;
    ah_error_clear(error);
    if (response)
        *response = NULL;
    if (!provider || !response)
        return ah_fail(error, AH_INVALID, "invalid provider request");
    if (provider->requests || provider->dispatching)
        return ah_fail(error, AH_BUSY, "provider is running");
    void *handle = NULL;
    ah_code code = start(context, json, delta, delta_context, cancelled, cancel_context,
                         &handle, error);
    if (code)
        return code;
    bool done = false;
    while (!done && !code) {
        code = poll_requests(context, 20, error);
        if (!code)
            code = result(context, handle, &done, response, error);
    }
    release(context, handle);
    return code;
}
