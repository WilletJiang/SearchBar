#include "agent_fixture.h"
#include <unistd.h>

void af_ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}

static void owner(agent_fixture *f) {
    assert(pthread_equal(f->owner, pthread_self()));
}

static ah_code start(void *context, const char *json, ah_delta_sink delta, void *delta_context,
                     ah_cancel_check cancelled, void *cancel_context, void **out, ah_error *error) {
    (void)error;
    agent_fixture *f = context;
    owner(f);
    assert(!cancelled(cancel_context) && f->count < AF_REQUESTS);
    agent_request *r = &f->requests[f->count++];
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    const char *session =
        yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "prompt_cache_key"));
    assert(session && strlen(session) == AH_ID_CAP - 1);
    memcpy(r->session, session, AH_ID_CAP);
    yyjson_doc_free(doc);
    r->request = strdup(json);
    assert(r->request);
    r->ordinal = af_session_calls(f, r->session);
    r->live = true;
    r->started = f->tick;
    r->ready = f->tick + (f->latency ? f->latency : 1);
    if (strcmp(r->session, f->root)) {
        f->child_started++;
        r->hold = f->barrier != 0;
    }
    f->active++;
    assert(f->active <= f->limit);
    if (f->active > f->peak)
        f->peak = f->active;
    r->response = f->reply(f, r);
    assert(r->response);
    r->delta = delta;
    r->delta_context = delta_context;
    if (delta)
        delta(delta_context, "fixture", 7);
    *out = r;
    return AH_OK;
}

static ah_code poll(void *context, unsigned timeout_ms, ah_error *error) {
    (void)timeout_ms;
    (void)error;
    agent_fixture *f = context;
    owner(f);
    assert(++f->tick < 10000); /* A deadlocked join fails without waiting for wall time. */
    if (f->barrier && f->child_started >= f->barrier)
        for (unsigned i = 0; i < f->count; i++)
            f->requests[i].hold = false;
    for (unsigned i = 0; i < f->count; i++) {
        agent_request *r = &f->requests[i];
        if (r->live && !r->hold && f->tick >= r->ready && !r->ready_emitted) {
            r->ready_emitted = true;
            if (r->delta)
                r->delta(r->delta_context, "ready", 5);
        }
    }
    return AH_OK;
}

static ah_code result(void *context, void *handle, bool *done, char **response, ah_error *error) {
    (void)error;
    agent_fixture *f = context;
    agent_request *r = handle;
    owner(f);
    assert(r->live && !r->transferred);
    *done = !r->hold && f->tick >= r->ready;
    *response = NULL;
    if (*done) {
        r->transferred = true;
        *response = r->response;
        r->response = NULL;
    }
    return AH_OK;
}

static void release(void *context, void *handle) {
    agent_fixture *f = context;
    agent_request *r = handle;
    owner(f);
    assert(r->live && f->active);
    r->live = false;
    r->released = f->tick;
    f->active--;
    free(r->response);
    r->response = NULL;
}

static ah_code synchronous(void *context, const char *request, ah_delta_sink delta,
                           void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                           char **response, ah_error *error) {
    (void)context;
    (void)request;
    (void)delta;
    (void)delta_context;
    (void)cancelled;
    (void)cancel_context;
    (void)response;
    (void)error;
    assert(!"agent scheduler unexpectedly used synchronous provider");
    return AH_PROTOCOL;
}

static void observe(void *context, const ah_event *event) {
    agent_fixture *f = context;
    owner(f);
    f->intents += !strcmp(event->type, "tool.intent");
    f->receipts += !strcmp(event->type, "tool.completed");
    f->spawned += !strcmp(event->type, "agent.spawned");
    f->completed += !strcmp(event->type, "model.completed");
    if (f->on_event)
        f->on_event(f, event);
    if (f->crash_event && !strcmp(f->crash_event, event->type))
        _exit(77);
}

void af_open(agent_fixture *f, unsigned max_active, unsigned max_agents, unsigned max_depth) {
    f->owner = pthread_self();
    f->limit = max_active;
    ah_config config;
    ah_config_init(&config);
    config.database_path = f->database ? f->database : ":memory:";
    config.provider = (ah_provider){synchronous, f};
    config.observer = observe;
    config.observer_context = f;
    config.max_model_calls = 100;
    config.max_tool_calls = 100;
    ah_error error = {0};
    af_ok(ah_open(&config, &f->engine, &error), &error);
    ah_agents_config agents;
    ah_agents_config_init(&agents);
    agents.provider = (ah_async_provider){f, start, poll, result, release};
    agents.max_active = max_active;
    agents.max_agents = max_agents;
    agents.max_depth = max_depth;
    af_ok(ah_agents_enable(f->engine, &agents, &error), &error);
}

void af_reopen(agent_fixture *f) {
    assert(!f->active);
    ah_close(f->engine);
    f->engine = NULL;
    af_open(f, f->limit, 16, 8);
}

void af_close(agent_fixture *f) {
    ah_close(f->engine);
    assert(!f->active);
    for (unsigned i = 0; i < f->count; i++) {
        assert(!f->requests[i].live && !f->requests[i].response);
        free(f->requests[i].request);
    }
}

void af_create(agent_fixture *f, const char *input) {
    ah_error error = {0};
    af_ok(ah_session_create(f->engine, "mock", "HOST_INSTRUCTIONS_914", f->root, &error), &error);
    af_ok(ah_submit(f->engine, f->root, "initial", input, &error), &error);
}
void af_run(agent_fixture *f) {
    ah_error error = {0};
    af_ok(ah_run(f->engine, f->root, &error), &error);
    assert(!f->active);
}
void af_state(agent_fixture *f, const char *session, const char *expected) {
    ah_error error = {0};
    char *json = NULL;
    af_ok(ah_session_json(f->engine, session, &json, &error), &error);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc && yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "state"), expected));
    yyjson_doc_free(doc);
    free(json);
}

char *af_text(const char *text) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d), *output = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_str(d, root, "status", "completed");
    yyjson_mut_obj_add_val(d, root, "output", output);
    yyjson_mut_val *item = yyjson_mut_obj(d), *content = yyjson_mut_arr(d),
                   *part = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, item, "type", "message");
    yyjson_mut_obj_add_str(d, item, "role", "assistant");
    yyjson_mut_obj_add_val(d, item, "content", content);
    yyjson_mut_obj_add_str(d, part, "type", "output_text");
    yyjson_mut_obj_add_str(d, part, "text", text);
    yyjson_mut_arr_append(content, part);
    yyjson_mut_arr_append(output, item);
    char *json = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return json;
}

char *af_calls(unsigned count, const char *const *names, const char *const *ids,
               const char *const *arguments) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d), *output = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_str(d, root, "status", "completed");
    yyjson_mut_obj_add_val(d, root, "output", output);
    for (unsigned i = 0; i < count; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, item, "type", "function_call");
        yyjson_mut_obj_add_str(d, item, "namespace", "local");
        yyjson_mut_obj_add_str(d, item, "name", names[i]);
        yyjson_mut_obj_add_str(d, item, "call_id", ids[i]);
        yyjson_mut_obj_add_str(d, item, "arguments", arguments[i]);
        yyjson_mut_arr_append(output, item);
    }
    char *json = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return json;
}
char *af_call(const char *name, const char *id, const char *arguments) {
    return af_calls(1, &name, &id, &arguments);
}
char *af_join(const char *call, const char *const *ids, unsigned count, unsigned timeout_ms) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d), *array = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_val(d, root, "ids", array);
    yyjson_mut_obj_add_uint(d, root, "timeout_ms", timeout_ms);
    for (unsigned i = 0; i < count; i++)
        yyjson_mut_arr_add_str(d, array, ids[i]);
    char *args = yyjson_mut_write(d, 0, NULL), *json = af_call("join", call, args);
    free(args);
    yyjson_mut_doc_free(d);
    return json;
}

yyjson_doc *af_receipt(const char *request, const char *call) {
    yyjson_doc *d = yyjson_read(request, strlen(request), 0), *found = NULL;
    assert(d);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(d), "input"), *item;
    size_t index, count;
    yyjson_arr_foreach(input, index, count, item) {
        if (!yyjson_equals_str(yyjson_obj_get(item, "type"), "function_call_output") ||
            !yyjson_equals_str(yyjson_obj_get(item, "call_id"), call))
            continue;
        assert(!found); /* A persisted exchange must have exactly one receipt. */
        const char *text = yyjson_get_str(yyjson_obj_get(item, "output"));
        assert(text);
        found = yyjson_read(text, strlen(text), 0);
        assert(found);
    }
    yyjson_doc_free(d);
    return found;
}
yyjson_val *af_result(yyjson_doc *doc) {
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_is_false(yyjson_obj_get(root, "is_error")));
    return yyjson_obj_get(root, "result");
}
void af_spawn_id(const char *request, const char *call, char id[AH_ID_CAP]) {
    yyjson_doc *doc = af_receipt(request, call);
    const char *value = yyjson_get_str(yyjson_obj_get(af_result(doc), "id"));
    assert(value && strlen(value) == AH_ID_CAP - 1);
    memcpy(id, value, AH_ID_CAP);
    yyjson_doc_free(doc);
}
void af_terminal_operation(const char *request, const char *child, char id[AH_ID_CAP]) {
    yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
    assert(doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input"), *item;
    size_t index, count;
    bool found = false;
    yyjson_arr_foreach(input, index, count, item) {
        const char *content = yyjson_get_str(yyjson_obj_get(item, "content"));
        const char *body = content ? strstr(content, "\n\n{") : NULL;
        if (!body)
            continue;
        yyjson_doc *message = yyjson_read(body + 2, strlen(body + 2), 0);
        yyjson_val *root = message ? yyjson_doc_get_root(message) : NULL;
        if (yyjson_equals_str(yyjson_obj_get(root, "agent_id"), child)) {
            const char *operation = yyjson_get_str(yyjson_obj_get(root, "operation_id"));
            assert(!found && operation && strlen(operation) == AH_ID_CAP - 1);
            memcpy(id, operation, AH_ID_CAP);
            found = true;
        }
        yyjson_doc_free(message);
    }
    yyjson_doc_free(doc);
    assert(found);
}
unsigned af_occurrences(const char *text, const char *needle) {
    unsigned count = 0;
    for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle))
        count++;
    return count;
}
unsigned af_session_calls(const agent_fixture *f, const char *session) {
    unsigned count = 0;
    for (unsigned i = 0; i < f->count; i++)
        count += !strcmp(f->requests[i].session, session);
    return count;
}
yyjson_doc *af_agents(agent_fixture *f) {
    char *json = NULL;
    ah_error error = {0};
    af_ok(ah_agents_json(f->engine, f->root, &json, &error), &error);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    free(json);
    return doc;
}
void af_remove_database(const char *path) {
    char sidecar[512];
    assert(unlink(path) == 0);
    const char *suffixes[] = {".lock", "-journal", "-wal", "-shm"};
    for (unsigned i = 0; i < 4; i++) {
        assert(snprintf(sidecar, sizeof(sidecar), "%s%s", path, suffixes[i]) > 0);
        unlink(sidecar);
    }
}
