#include "ah/responses.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pthread_t owner;
    ah_responses *provider;
    ah_responses_metrics receipts[32];
    size_t count;
} fixture;

typedef struct {
    fixture *fixture;
    char json[256], delta[64];
    size_t length;
    void *handle;
    char *response;
    uint64_t finished;
    ah_code code;
    bool done, cancel_on_delta;
} request;

static bool cancelled(void *context) {
    request *r = context;
    assert(pthread_equal(pthread_self(), r->fixture->owner));
    return r->cancel_on_delta && r->length;
}

static void delta(void *context, const char *bytes, size_t length) {
    request *r = context;
    assert(pthread_equal(pthread_self(), r->fixture->owner));
    assert(length < sizeof(r->delta) - r->length);
    memcpy(r->delta + r->length, bytes, length);
    r->length += length;
    r->delta[r->length] = 0;
}

static void observe(void *context, const ah_responses_metrics *metrics) {
    fixture *f = context;
    assert(pthread_equal(pthread_self(), f->owner));
    assert(f->count < sizeof(f->receipts) / sizeof(f->receipts[0]));
    f->receipts[f->count++] = *metrics;
    ah_error error;
    ah_provider sync = ah_responses_provider(f->provider);
    ah_async_provider async = ah_responses_async_provider(f->provider);
    char *json = NULL;
    void *handle = NULL;
    assert(ah_responses_set_token(f->provider, "must-not-replace", &error) == AH_BUSY);
    assert(sync.complete(sync.context, "{}", NULL, NULL, NULL, NULL, &json, &error) == AH_BUSY);
    assert(async.start(async.context, "{}", NULL, NULL, NULL, NULL, &handle, &error) == AH_BUSY);
    assert(async.poll(async.context, 0, &error) == AH_BUSY);
    assert(!json && !handle);
}

static void begin(ah_async_provider async, request *r, size_t index) {
    char route[64] = "";
    if (!(index % 2))
        snprintf(route, sizeof(route), ",\"prompt_cache_key\":\"route-%zu\"", index);
    else if (index % 4 == 3)
        snprintf(route, sizeof(route), ",\"prompt_cache_key\":null");
    snprintf(r->json, sizeof(r->json),
             "{\"model\":\"test\",\"input\":[],\"stream\":true,"
             "\"tag\":\"request-%zu\"%s}",
             index, route);
    ah_error error;
    assert(async.start(async.context, r->json, delta, r, cancelled, r, &r->handle, &error) ==
           AH_OK);
    assert(r->handle);
    bool done = true;
    char *json = (char *)1;
    assert(async.result(async.context, r->handle, &done, &json, &error) == AH_OK);
    assert(!done && !json);
}

int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    const char *mode = argv[2];
    size_t count = !strcmp(mode, "overlap") ? 8 : 2;
    bool stagger = !strcmp(mode, "deadline");
    fixture f = {.owner = pthread_self()};
    ah_responses_config config;
    ah_responses_config_init(&config);
    config.endpoint = argv[1];
    config.bearer_token = "local-test-token";
    config.allow_loopback_http = true;
    config.timeout_ms = stagger ? 180 : 3000;
    config.observe = observe;
    config.observe_context = &f;
    ah_error error;
    assert(ah_responses_open(&config, &f.provider, &error) == AH_OK);
    ah_async_provider async = ah_responses_async_provider(f.provider);
    void *invalid = (void *)1;
    assert(async.start(async.context, "{\"prompt_cache_key\":\"bad route\"}", NULL, NULL, NULL,
                       NULL, &invalid, &error) == AH_INVALID);
    assert(!invalid && !f.count);
    assert(async.start(async.context, "invalid-json", NULL, NULL, NULL, NULL, &invalid, &error) ==
           AH_INVALID);
    assert(!invalid && !f.count);
    assert(ah_responses_set_token(f.provider, "local-test-token", &error) == AH_OK);
    request requests[8] = {0};
    uint64_t started = ah_now_ms();
    size_t begun = stagger ? 1 : count;
    for (size_t i = 0; i < count; i++) {
        requests[i].fixture = &f;
        requests[i].cancel_on_delta = i == 0 && !strcmp(mode, "cancel");
        if (i < begun)
            begin(async, &requests[i], i);
    }
    size_t remaining = count;
    while (remaining) {
        assert(ah_now_ms() - started < 5000);
        assert(async.poll(async.context, 20, &error) == AH_OK);
        if (stagger && begun == 1 && ah_now_ms() - started >= 100) {
            begin(async, &requests[1], 1);
            begun++;
        }
        for (size_t i = 0; i < begun; i++) {
            request *r = &requests[i];
            if (r->done)
                continue;
            if (i == 0 && !strcmp(mode, "release") && r->length) {
                async.release(async.context, r->handle);
                r->handle = NULL;
                r->done = true;
                r->code = AH_CANCELLED;
            } else {
                r->code = async.result(async.context, r->handle, &r->done, &r->response, &error);
                assert(r->done || r->code == AH_OK);
            }
            if (r->done) {
                r->finished = ah_now_ms() - started;
                remaining--;
            }
        }
    }
    /* A completed handle still pins the token and borrowed request lifetime. */
    assert(ah_responses_set_token(f.provider, "must-not-replace", &error) == AH_BUSY);
    ah_provider sync = ah_responses_provider(f.provider);
    char *json = NULL;
    assert(sync.complete(sync.context, "{}", NULL, NULL, NULL, NULL, &json, &error) == AH_BUSY);
    for (size_t i = 0; i < count; i++) {
        bool done = false;
        if (requests[i].handle) {
            ah_code expected = requests[i].code ? requests[i].code : AH_INVALID;
            assert(async.result(async.context, requests[i].handle, &done, &json, &error) ==
                   expected);
            assert(done && !json);
        }
        async.release(async.context, requests[i].handle);
        assert(async.result(async.context, requests[i].handle, &done, &json, &error) == AH_INVALID);
        assert(!done && !json);
    }
    assert(ah_responses_set_token(f.provider, "replacement-token", &error) == AH_OK);
    size_t receipts = f.count;
    uint64_t idle = ah_now_ms();
    assert(async.poll(async.context, 20, &error) == AH_OK);
    assert(ah_now_ms() - idle >= 10);
    assert(receipts == f.count);
    printf("{\"elapsed\":%llu,\"requests\":[", (unsigned long long)(ah_now_ms() - started));
    for (size_t i = 0; i < count; i++) {
        request *r = &requests[i];
        printf("%s{\"code\":%d,\"delta\":\"%s\",\"finished\":%llu,\"response\":%s}", i ? "," : "",
               r->code, r->delta, (unsigned long long)r->finished,
               r->response ? r->response : "null");
        free(r->response);
    }
    printf("],\"metrics\":[");
    for (size_t i = 0; i < f.count; i++) {
        const ah_responses_metrics *m = &f.receipts[i];
        printf("%s{\"attempt\":%u,\"result\":%d,\"retry\":%s,\"connections\":%ld}", i ? "," : "",
               m->attempt, m->result, m->retry_planned ? "true" : "false", m->new_connections);
    }
    puts("]}");
    ah_responses_close(f.provider);
    return 0;
}
