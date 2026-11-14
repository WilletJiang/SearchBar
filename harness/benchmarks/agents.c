/* Synthetic asynchronous model latency; no network, API key, or inference. */
#include <ah/agents.h>
#include <yyjson.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHILDREN 4
#define LATENCY_MS 20
#define SAMPLES 8
#define WARMUPS 1

typedef struct request {
    struct request *next;
    char *response;
    double ready;
    bool returned;
} request;
typedef struct {
    request *pending;
    char root[AH_ID_CAP];
    unsigned active, peak, starts, models, intents, receipts;
} fixture;

static void check(bool valid, const char *message) {
    if (!valid) {
        fprintf(stderr, "agent benchmark: %s\n", message);
        exit(1);
    }
}
static double now_ms(void) {
    struct timespec value;
    check(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "cannot read monotonic clock");
    return (double)value.tv_sec * 1000 + (double)value.tv_nsec / 1000000;
}
static void ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        exit(1);
    }
}
static char *done(const char *text) {
    char output[512];
    int size = snprintf(output, sizeof(output),
                        "{\"status\":\"completed\",\"output\":["
                        "{\"type\":\"message\",\"role\":\"assistant\",\"content\":["
                        "{\"type\":\"output_text\",\"text\":\"%s\"}]}]}",
                        text);
    check(size > 0 && (size_t)size < sizeof(output), "response overflow");
    return strdup(output);
}
static yyjson_doc *receipt(yyjson_val *input, const char *call) {
    yyjson_val *item;
    size_t index, count;
    yyjson_doc *found = NULL;
    yyjson_arr_foreach(input, index, count, item) {
        if (!yyjson_equals_str(yyjson_obj_get(item, "type"), "function_call_output") ||
            !yyjson_equals_str(yyjson_obj_get(item, "call_id"), call))
            continue;
        check(!found, "duplicate tool receipt");
        const char *text = yyjson_get_str(yyjson_obj_get(item, "output"));
        found = text ? yyjson_read(text, strlen(text), 0) : NULL;
        check(found && yyjson_is_false(yyjson_obj_get(yyjson_doc_get_root(found), "is_error")),
              "missing or failed tool receipt");
    }
    return found;
}
static void function(yyjson_mut_doc *doc, yyjson_mut_val *output, const char *name, const char *id,
                     const char *arguments) {
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, item, "type", "function_call");
    yyjson_mut_obj_add_str(doc, item, "namespace", "local");
    yyjson_mut_obj_add_str(doc, item, "name", name);
    yyjson_mut_obj_add_strcpy(doc, item, "call_id", id);
    yyjson_mut_obj_add_strcpy(doc, item, "arguments", arguments);
    yyjson_mut_arr_append(output, item);
}
static char *coordinate(yyjson_val *input, bool spawned) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    check(doc != NULL, "out of memory");
    yyjson_mut_val *root = yyjson_mut_obj(doc), *output = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "completed");
    yyjson_mut_obj_add_val(doc, root, "output", output);
    char args[512] = "{\"ids\":[";
    size_t used = strlen(args);
    for (unsigned i = 0; i < CHILDREN; i++) {
        char call[32], task[128];
        snprintf(call, sizeof(call), "spawn_%u", i);
        if (!spawned) {
            snprintf(task, sizeof(task), "{\"name\":\"worker_%u\",\"message\":\"TASK_%u\"}", i, i);
            function(doc, output, "spawn", call, task);
            continue;
        }
        yyjson_doc *found = receipt(input, call);
        yyjson_val *result = found ? yyjson_obj_get(yyjson_doc_get_root(found), "result") : NULL;
        const char *id = yyjson_get_str(yyjson_obj_get(result, "id"));
        check(id && strlen(id) == AH_ID_CAP - 1, "missing spawned agent identity");
        int size = snprintf(args + used, sizeof(args) - used, "%s\"%s\"", i ? "," : "", id);
        check(size > 0 && (size_t)size < sizeof(args) - used, "join overflow");
        used += (size_t)size;
        yyjson_doc_free(found);
    }
    if (spawned) {
        check(snprintf(args + used, sizeof(args) - used, "],\"timeout_ms\":10000}") > 0,
              "cannot encode join");
        function(doc, output, "join", "join_all", args);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}
static char *respond(fixture *f, const char *json) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    check(doc != NULL, "invalid model request");
    yyjson_val *root = yyjson_doc_get_root(doc), *input = yyjson_obj_get(root, "input");
    const char *id = yyjson_get_str(yyjson_obj_get(root, "prompt_cache_key"));
    check(id != NULL, "missing session identity");
    char *response = NULL;
    if (strcmp(id, f->root)) {
        check(yyjson_arr_size(input) == 1, "child context was inherited");
        for (unsigned i = 0; i < CHILDREN; i++) {
            char task[32], output[32];
            snprintf(task, sizeof(task), "TASK_%u", i);
            snprintf(output, sizeof(output), "RESULT_%u", i);
            if (strstr(json, task))
                response = done(output);
        }
        check(response != NULL, "child received incorrect task");
    } else {
        yyjson_doc *spawn = receipt(input, "spawn_0"), *join = receipt(input, "join_all");
        if (!join)
            response = coordinate(input, spawn != NULL);
        else {
            yyjson_val *result = yyjson_obj_get(yyjson_doc_get_root(join), "result");
            check(yyjson_is_false(yyjson_obj_get(result, "timed_out")), "join timed out");
            for (unsigned i = 0; i < CHILDREN; i++) {
                char expected[32];
                snprintf(expected, sizeof(expected), "RESULT_%u", i);
                const char *text = strstr(json, expected);
                check(text && !strstr(text + strlen(expected), expected),
                      "child result absent or delivered more than once");
            }
            response = done("VERIFIED");
        }
        yyjson_doc_free(spawn);
        yyjson_doc_free(join);
    }
    yyjson_doc_free(doc);
    check(response != NULL, "out of memory");
    return response;
}
static ah_code start(void *context, const char *json, ah_delta_sink delta, void *delta_context,
                     ah_cancel_check cancelled, void *cancel_context, void **out, ah_error *error) {
    (void)delta;
    (void)delta_context;
    (void)error;
    fixture *f = context;
    check(!cancelled(cancel_context), "unexpected cancellation");
    request *r = calloc(1, sizeof(*r));
    check(r != NULL, "out of memory");
    r->response = respond(f, json);
    r->ready = now_ms() + LATENCY_MS;
    r->next = f->pending;
    f->pending = r;
    f->starts++;
    if (++f->active > f->peak)
        f->peak = f->active;
    *out = r;
    return AH_OK;
}
static ah_code poll(void *context, unsigned timeout, ah_error *error) {
    (void)error;
    fixture *f = context;
    double current = now_ms(), wait = timeout;
    for (request *r = f->pending; r; r = r->next)
        if (r->ready - current < wait)
            wait = r->ready - current;
    if (wait > 0) {
        struct timespec pause = {.tv_sec = (time_t)(wait / 1000),
                                 .tv_nsec = (long)(wait * 1000000) % 1000000000L};
        while (nanosleep(&pause, &pause) < 0)
            check(errno == EINTR, "cannot wait for synthetic response");
    }
    return AH_OK;
}
static ah_code result(void *context, void *handle, bool *complete, char **output, ah_error *error) {
    (void)context;
    (void)error;
    request *r = handle;
    check(!r->returned, "model response requested twice");
    *complete = now_ms() >= r->ready;
    *output = NULL;
    if (*complete) {
        r->returned = true;
        *output = r->response;
        r->response = NULL;
    }
    return AH_OK;
}
static void release(void *context, void *handle) {
    fixture *f = context;
    request **next = &f->pending;
    while (*next && *next != handle)
        next = &(*next)->next;
    check(*next != NULL && f->active != 0, "unknown request released");
    request *r = *next;
    *next = r->next;
    free(r->response);
    free(r);
    f->active--;
}
static ah_code synchronous(void *context, const char *json, ah_delta_sink delta, void *delta_ctx,
                           ah_cancel_check cancel, void *cancel_ctx, char **out, ah_error *error) {
    (void)context;
    (void)json;
    (void)delta;
    (void)delta_ctx;
    (void)cancel;
    (void)cancel_ctx;
    (void)out;
    (void)error;
    check(false, "synchronous provider unexpectedly used");
    return AH_PROTOCOL;
}
static void observe(void *context, const ah_event *event) {
    fixture *f = context;
    f->models += !strcmp(event->type, "model.completed");
    f->intents += !strcmp(event->type, "tool.intent");
    f->receipts += !strcmp(event->type, "tool.completed");
}
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void benchmark(unsigned limit) {
    double samples[SAMPLES], sorted[SAMPLES];
    unsigned observed_peak = 0;
    for (unsigned run = 0; run < SAMPLES + WARMUPS; run++) {
        fixture f = {0};
        ah_config config;
        ah_config_init(&config);
        config.database_path = ":memory:";
        config.provider = (ah_provider){synchronous, &f};
        config.observer = observe;
        config.observer_context = &f;
        ah_engine *engine = NULL;
        ah_error error = {0};
        ok(ah_open(&config, &engine, &error), &error);
        ah_agents_config agents;
        ah_agents_config_init(&agents);
        agents.provider = (ah_async_provider){&f, start, poll, result, release};
        agents.max_active = limit;
        ok(ah_agents_enable(engine, &agents, &error), &error);
        ok(ah_session_create(engine, "synthetic", "", f.root, &error), &error);
        double began = now_ms();
        ok(ah_submit(engine, f.root, "benchmark", "Coordinate four tasks.", &error), &error);
        ok(ah_run(engine, f.root, &error), &error);
        double elapsed = now_ms() - began;
        check(!f.active && f.starts == 7 && f.models == 7 && f.intents == 5 && f.receipts == 5,
              "incorrect model calls or durable coordination receipts");
        check(f.peak == limit, "requested model concurrency was not exercised");
        observed_peak = f.peak;
        ah_close(engine);
        if (run >= WARMUPS)
            samples[run - WARMUPS] = elapsed;
    }
    memcpy(sorted, samples, sizeof(sorted));
    qsort(sorted, SAMPLES, sizeof(*sorted), compare);
    printf("{\"workload\":\"four_fresh_context_agents\",\"store\":\"memory\",\"max_active\":%u,"
           "\"observed_peak\":%u,\"runs\":%u,\"warmups\":%u,\"model_latency_ms\":%u,"
           "\"model_requests_per_run\":7,\"coordination_receipts_per_run\":5,"
           "\"timed\":\"submit+run\",\"network\":false,\"model_inference\":false,"
           "\"content_and_receipts_checked\":true,\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"raw_ms\":[",
           limit, observed_peak, SAMPLES, WARMUPS, LATENCY_MS, sorted[SAMPLES / 2 - 1],
           sorted[SAMPLES - 1]);
    for (unsigned i = 0; i < SAMPLES; i++)
        printf("%s%.3f", i ? "," : "", samples[i]);
    puts("]}");
}
int main(void) {
    benchmark(1);
    benchmark(4);
    return 0;
}
