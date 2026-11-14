#include "responses_internal.h"
#include "stream.h"
#include <stdio.h>
#include <strings.h>

ah_code responses_check_stop(responses_request *t) {
    if (t->error.code)
        return t->error.code;
    if (t->cancelled && t->cancelled(t->cancel_context))
        return ah_fail(&t->error, AH_CANCELLED, "provider request cancelled");
    if (ah_now_ms() >= t->deadline)
        return ah_fail(&t->error, AH_LIMIT, "provider deadline exceeded");
    return AH_OK;
}

static int progress(void *context, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a;
    (void)b;
    responses_request *t = context;
    if (t->measure && c > 0 && d >= c)
        responses_stamp(&t->measurement.metrics.upload_complete, t->measurement.started);
    return responses_check_stop(context) != AH_OK;
}

static size_t header(char *bytes, size_t size, size_t count, void *context) {
    responses_request *t = context;
    if (!size || count > SIZE_MAX / size)
        return 0;
    size_t length = size * count;
    if (t->measure && length)
        responses_stamp(&t->measurement.metrics.first_byte, t->measurement.started);
    if (responses_check_stop(t))
        return 0;
    if (length > 65536 - t->header_bytes) {
        ah_fail(&t->error, AH_LIMIT, "HTTP header limit exceeded");
        return 0;
    }
    t->header_bytes += length;
    if (length >= 5 && !memcmp(bytes, "HTTP/", 5)) {
        const char *space = memchr(bytes, ' ', length);
        if (!space || bytes + length - space < 4 || space[1] < '1' || space[1] > '5' ||
            space[2] < '0' || space[2] > '9' || space[3] < '0' || space[3] > '9') {
            ah_fail(&t->error, AH_PROTOCOL, "invalid HTTP status line");
            return 0;
        }
        t->status = (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
        t->content_type = false;
        t->content_type_seen = false;
        t->header_complete = false;
        t->retry_after = 0;
    } else if ((length == 2 && bytes[0] == '\r' && bytes[1] == '\n') ||
               (length == 1 && bytes[0] == '\n')) {
        t->header_complete = true;
    } else if (length >= 13 && !strncasecmp(bytes, "Content-Type:", 13)) {
        if (t->content_type_seen) {
            ah_fail(&t->error, AH_PROTOCOL, "duplicate Content-Type header");
            return 0;
        }
        t->content_type_seen = true;
        size_t offset = 13;
        while (offset < length && (bytes[offset] == ' ' || bytes[offset] == '\t'))
            offset++;
        const char *expected = "text/event-stream";
        size_t n = strlen(expected);
        t->content_type =
            length >= offset + n && !strncasecmp(bytes + offset, expected, n) &&
            (length == offset + n || bytes[offset + n] == ';' || bytes[offset + n] == '\r' ||
             bytes[offset + n] == '\n' || bytes[offset + n] == ' ');
    } else if (length >= 12 && !strncasecmp(bytes, "Retry-After:", 12)) {
        size_t offset = 12;
        while (offset < length && bytes[offset] == ' ')
            offset++;
        unsigned seconds = 0;
        while (offset < length && bytes[offset] >= '0' && bytes[offset] <= '9') {
            seconds = seconds * 10 + (unsigned)(bytes[offset++] - '0');
            if (seconds > 30) {
                seconds = 30;
                break;
            }
        }
        t->retry_after = seconds * 1000;
    }
    return length;
}

static size_t body(char *bytes, size_t size, size_t count, void *context) {
    responses_request *t = context;
    if (!size || count > SIZE_MAX / size)
        return 0;
    size_t length = size * count;
    if (t->measure) {
        t->measurement.metrics.response_bytes += length;
        if (length)
            responses_stamp(&t->measurement.metrics.first_byte, t->measurement.started);
    }
    if (responses_check_stop(t))
        return 0;
    if (length > t->limit - t->received) {
        ah_fail(&t->error, AH_LIMIT, "response byte limit exceeded");
        return 0;
    }
    t->received += length;
    if (t->status < 200 || t->status >= 300)
        return length;
    if (!t->header_complete || (t->content_type_seen && !t->content_type)) {
        ah_fail(&t->error, AH_PROTOCOL, "expected text/event-stream response");
        return 0;
    }
    if (ah_stream_feed(&t->stream, bytes, length)) {
        t->error = t->stream.error;
        return 0;
    }
    return length;
}

static bool retryable(CURLcode code) {
    return code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
           code == CURLE_SEND_ERROR || code == CURLE_RECV_ERROR || code == CURLE_GOT_NOTHING ||
           code == CURLE_PARTIAL_FILE || code == CURLE_HTTP2 || code == CURLE_HTTP2_STREAM;
}

/* ChatGPT cache affinity uses this header independently of the JSON key. Derive
 * it from each request, not provider state: one transport can serve many sessions. */
static bool request_route(const char *json, char route[525]) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool valid = yyjson_is_obj(root) && ah_json_check(root, NULL) == AH_OK;
    route[0] = 0;
    yyjson_val *key = valid ? yyjson_obj_get(root, "prompt_cache_key") : NULL;
    if (key && !yyjson_is_null(key)) {
        const char *value = yyjson_get_str(key);
        size_t length = yyjson_get_len(key);
        valid = value && length && length <= 512;
        for (size_t i = 0; valid && i < length; i++)
            valid = (unsigned char)value[i] >= 0x21 && (unsigned char)value[i] <= 0x7e;
        if (valid)
            snprintf(route, 525, "session-id: %s", value);
    }
    yyjson_doc_free(doc);
    return valid;
}

ah_code responses_request_setup(responses_request *t, ah_error *error) {
    ah_responses *provider = t->provider;
    char route[525];
    if (!request_route(t->json, route))
        return ah_fail(error, AH_INVALID, "invalid provider request");
    size_t auth_size = strlen(provider->token) + 23;
    char *auth = malloc(auth_size);
    if (!auth)
        return ah_fail(error, AH_IO, "provider allocation failed");
    snprintf(auth, auth_size, "Authorization: Bearer %s", provider->token);
    t->headers = curl_slist_append(NULL, auth);
    responses_wipe(auth);
    free(auth);
    if (!t->headers)
        goto failed;
    const char *extra[] = {"Content-Type: application/json", "Accept: text/event-stream", route};
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
        if (!*extra[i])
            continue;
        struct curl_slist *next = curl_slist_append(t->headers, extra[i]);
        if (!next)
            goto failed;
        t->headers = next;
    }
    t->easy = curl_easy_init();
    if (!t->easy)
        goto failed;
#define SET(option, value)                                                                         \
    do {                                                                                           \
        if (curl_easy_setopt(t->easy, option, value) != CURLE_OK)                                  \
            goto failed;                                                                           \
    } while (0)
    SET(CURLOPT_URL, provider->endpoint);
    SET(CURLOPT_HTTPHEADER, t->headers);
    SET(CURLOPT_POSTFIELDS, t->json);
    SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)t->request_bytes);
    SET(CURLOPT_POST, 1L);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, progress);
    SET(CURLOPT_XFERINFODATA, t);
    SET(CURLOPT_WRITEFUNCTION, body);
    SET(CURLOPT_WRITEDATA, t);
    SET(CURLOPT_HEADERFUNCTION, header);
    SET(CURLOPT_HEADERDATA, t);
    SET(CURLOPT_PRIVATE, t);
    if (provider->loopback)
        SET(CURLOPT_PROXY, "");
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, provider->config.allow_loopback_http ? "https,http" : "https");
#else
    SET(CURLOPT_PROTOCOLS,
        (long)(CURLPROTO_HTTPS | (provider->config.allow_loopback_http ? CURLPROTO_HTTP : 0)));
#endif
#undef SET
    return AH_OK;
failed:
    return ah_fail(error, AH_IO, "provider allocation or setup failed");
}

ah_code responses_attempt_start(responses_request *t) {
    ah_responses *provider = t->provider;
    uint64_t now = ah_now_ms();
    uint64_t remaining = now < t->deadline ? t->deadline - now : 1;
    if (curl_easy_setopt(t->easy, CURLOPT_TIMEOUT_MS, (long)remaining) != CURLE_OK ||
        curl_multi_add_handle(provider->multi, t->easy) != CURLM_OK)
        return ah_fail(&t->error, AH_IO, "provider attempt setup failed");
    t->active = true;
    t->attempt++;
    t->status = 0;
    t->received = t->header_bytes = 0;
    t->retry_after = 0;
    t->content_type = t->content_type_seen = t->header_complete = false;
    t->error = (ah_error){0};
    t->measurement = (responses_measurement){0};
    ah_stream_init(&t->stream, provider->config.max_event_bytes, t->limit, t->delta,
                   t->delta_context);
    if (t->measure) {
        t->measurement.metrics.attempt = t->attempt;
        t->measurement.metrics.request_bytes = t->request_bytes;
        t->measurement.started = ah_now_us();
        t->stream.phase = responses_phase;
        t->stream.phase_context = &t->measurement;
    }
    return AH_OK;
}

void responses_attempt_finish(responses_request *t, CURLcode curl_result) {
    ah_responses *provider = t->provider;
    curl_multi_remove_handle(provider->multi, t->easy);
    t->active = false;
    bool local_failure = t->error.code != AH_OK;
    ah_code result = t->error.code;
    if (result) {
        /* Cancellation, limits and parser errors take precedence over curl. */
    } else if (t->status == 401 || t->status == 403)
        result = ah_fail(&t->error, AH_AUTH, "provider authentication failed");
    else if (t->status == 429)
        result = ah_fail(&t->error, AH_LIMIT, "provider rate or usage limit");
    else if (t->status >= 500)
        result = ah_fail(&t->error, AH_NETWORK, "provider HTTP %ld", t->status);
    else if (curl_result == CURLE_OPERATION_TIMEDOUT)
        result = ah_fail(&t->error, AH_LIMIT, "provider deadline exceeded");
    else if (curl_result != CURLE_OK)
        result =
            ah_fail(&t->error, AH_NETWORK, "provider transport failure (%d)", (int)curl_result);
    else if (t->status < 200 || t->status >= 300)
        result = ah_fail(&t->error, AH_PROTOCOL, "provider HTTP %ld", t->status);
    else if (t->content_type_seen && !t->content_type)
        result = ah_fail(&t->error, AH_PROTOCOL, "expected text/event-stream response");
    else {
        result = ah_stream_finish(&t->stream, &t->response);
        t->error = t->stream.error;
    }
    bool retry = result && !local_failure && result != AH_CANCELLED && result != AH_PROTOCOL &&
                 result != AH_AUTH && !t->stream.observed_output && !t->stream.terminal &&
                 t->attempt <= provider->config.max_retries &&
                 (t->status == 429 || t->status >= 500 || retryable(curl_result)) &&
                 ah_now_ms() < t->deadline;
    if (t->measure) {
        t->measurement.metrics.result = result;
        t->measurement.metrics.http_status = t->status;
        t->measurement.metrics.retry_planned = retry;
        t->measurement.metrics.terminal_completed = t->stream.terminal;
        responses_observe(provider, t->easy, &t->measurement);
    }
    ah_stream_destroy(&t->stream);
    t->done = !retry;
    if (retry) {
        unsigned delay = t->retry_after ? t->retry_after : 200U << (t->attempt - 1);
        t->retry_at = ah_now_ms() + delay;
        t->error = (ah_error){0};
    }
}

void responses_request_free(responses_request *t) {
    if (!t)
        return;
    curl_easy_cleanup(t->easy);
    for (struct curl_slist *item = t->headers; item; item = item->next)
        responses_wipe(item->data);
    curl_slist_free_all(t->headers);
    ah_stream_destroy(&t->stream);
    free(t->response);
    free(t);
}
