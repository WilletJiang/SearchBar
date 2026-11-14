#include "agent_fixture.h"

static void fresh_request(agent_request *r) {
    yyjson_doc *doc = yyjson_read(r->request, strlen(r->request), 0);
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_equals_str(yyjson_obj_get(root, "instructions"), "HOST_INSTRUCTIONS_914"));
    assert(yyjson_equals_str(yyjson_obj_get(root, "model"), "mock"));
    assert(!strstr(r->request, "ROOT_PRIVATE") && !strstr(r->request, "HISTORY_PRIVATE"));
    assert(!strstr(r->request, "ROOT_SUMMARY"));
    if (r->ordinal == 1)
        assert(yyjson_arr_size(yyjson_obj_get(root, "input")) == 1);
    yyjson_doc_free(doc);
}

static void valid_join(const char *request, const char *call, unsigned count) {
    yyjson_doc *doc = af_receipt(request, call);
    yyjson_val *result = af_result(doc);
    assert(yyjson_is_false(yyjson_obj_get(result, "timed_out")));
    yyjson_val *agents = yyjson_obj_get(result, "agents"), *agent;
    assert(yyjson_arr_size(agents) == count);
    size_t i, n;
    yyjson_arr_foreach(agents, i, n, agent) {
        if (!yyjson_equals_str(yyjson_obj_get(agent, "state"), "completed"))
            fprintf(stderr, "%s child state: %s\n", call,
                    yyjson_get_str(yyjson_obj_get(agent, "state")));
        assert(yyjson_equals_str(yyjson_obj_get(agent, "state"), "completed"));
    }
    yyjson_doc_free(doc);
}

static void error_receipt(const char *request, const char *call) {
    yyjson_doc *doc = af_receipt(request, call);
    assert(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "is_error")));
    yyjson_doc_free(doc);
}

static char *batch_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        fresh_request(r);
        if (strstr(r->request, "TASK_ALPHA"))
            return af_text("RESULT_ALPHA");
        return af_text(strstr(r->request, "TASK_BETA") ? "RESULT_BETA" : "RESULT_GAMMA");
    }
    if (!strstr(r->request, "TASK_START"))
        return af_text("HISTORY_PRIVATE");
    yyjson_doc *receipt = af_receipt(r->request, "spawn_a");
    if (!receipt) {
        const char *names[] = {"spawn", "spawn", "spawn"};
        const char *ids[] = {"spawn_a", "spawn_b", "spawn_c"};
        const char *args[] = {"{\"name\":\"alpha\",\"message\":\"TASK_ALPHA\"}",
                              "{\"name\":\"beta\",\"message\":\"TASK_BETA\"}",
                              "{\"name\":\"gamma\",\"message\":\"TASK_GAMMA\"}"};
        return af_calls(3, names, ids, args);
    }
    yyjson_doc_free(receipt);
    af_spawn_id(r->request, "spawn_a", f->ids[0]);
    af_spawn_id(r->request, "spawn_b", f->ids[1]);
    af_spawn_id(r->request, "spawn_c", f->ids[2]);
    receipt = af_receipt(r->request, "join_all");
    if (!receipt) {
        const char *ids[] = {f->ids[0], f->ids[1], f->ids[2]};
        return af_join("join_all", ids, 3, 10000);
    }
    yyjson_doc_free(receipt);
    valid_join(r->request, "join_all", 3);
    assert(af_occurrences(r->request, "RESULT_ALPHA") == 1);
    assert(af_occurrences(r->request, "RESULT_BETA") == 1);
    assert(af_occurrences(r->request, "RESULT_GAMMA") == 1);
    return af_text("ROOT_DONE");
}

static void concurrent_fresh_children(void) {
    agent_fixture f = {.reply = batch_reply, .barrier = 3, .latency = 2};
    af_open(&f, 4, 8, 4);
    af_create(&f, "HISTORY_ONLY ROOT_PRIVATE");
    af_run(&f);
    ah_error error = {0};
    af_ok(ah_compact(f.engine, f.root, "ROOT_SUMMARY", 1, &error), &error);
    af_ok(ah_submit(f.engine, f.root, "delegate", "TASK_START ROOT_PRIVATE", &error), &error);
    af_run(&f);
    assert(f.peak >= 3 && f.peak <= 4 && f.child_started == 3);
    assert(f.spawned == 3 && f.intents == 4 && f.receipts == 4 && f.completed == 7);
    for (unsigned i = 0; i < 3; i++) {
        af_state(&f, f.ids[i], "completed");
        assert(af_session_calls(&f, f.ids[i]) == 1);
    }
    af_state(&f, f.root, "completed");
    af_close(&f);
}

static char *nested_reply(agent_fixture *f, agent_request *r) {
    bool root = !strcmp(r->session, f->root);
    if (!root)
        fresh_request(r);
    yyjson_doc *request = yyjson_read(r->request, strlen(r->request), 0);
    assert(request);
    yyjson_val *first = yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(request), "input"));
    const char *initial = yyjson_get_str(yyjson_obj_get(first, "content"));
    bool leaf = initial && strstr(initial, "TASK_LEAF");
    yyjson_doc_free(request);
    if (leaf)
        return af_text("LEAF_RESULT");
    yyjson_doc *receipt = af_receipt(r->request, "spawn_nested");
    if (!receipt)
        return af_call("spawn", "spawn_nested",
                       root ? "{\"name\":\"branch\",\"message\":\"TASK_BRANCH\"}"
                            : "{\"name\":\"leaf\",\"message\":\"TASK_LEAF\"}");
    yyjson_doc_free(receipt);
    char child[AH_ID_CAP];
    af_spawn_id(r->request, "spawn_nested", child);
    memcpy(f->ids[root ? 0 : 1], child, AH_ID_CAP);
    receipt = af_receipt(r->request, "join_nested");
    if (!receipt) {
        const char *ids[] = {child};
        return af_join("join_nested", ids, 1, 10000);
    }
    yyjson_doc_free(receipt);
    valid_join(r->request, "join_nested", 1);
    assert(strstr(r->request, root ? "BRANCH_RESULT" : "LEAF_RESULT"));
    return af_text(root ? "ROOT_DONE" : "BRANCH_RESULT");
}

static void nested_join_with_one_slot(void) {
    agent_fixture f = {.reply = nested_reply};
    af_open(&f, 1, 8, 4);
    af_create(&f, "NESTED_ROOT");
    af_run(&f);
    assert(f.peak == 1 && f.count == 7 && f.spawned == 2);
    af_state(&f, f.root, "completed");
    af_state(&f, f.ids[0], "completed");
    af_state(&f, f.ids[1], "completed");
    af_close(&f);
}

static char *mail_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        fresh_request(r);
        if (r->ordinal > 1) {
            assert(af_occurrences(r->request, "QUEUED_ONLY") == 1);
            assert(af_occurrences(r->request, "SIBLING_DATA") == 1);
            assert(af_occurrences(r->request, "FOLLOWUP_WAKE") == 1);
        }
        return af_text("CHILD_DONE");
    }
    if (r->ordinal == 1) {
        const char *names[] = {"spawn", "spawn"}, *ids[] = {"mail_a", "mail_b"};
        const char *args[] = {"{\"name\":\"mail-a\",\"message\":\"TASK_MAIL_A\"}",
                              "{\"name\":\"mail-b\",\"message\":\"TASK_MAIL_B\"}"};
        return af_calls(2, names, ids, args);
    }
    af_spawn_id(r->request, "mail_a", f->ids[0]);
    af_spawn_id(r->request, "mail_b", f->ids[1]);
    return af_text("ROOT_DONE");
}

static void durable_mail_and_boundaries(void) {
    agent_fixture f = {.reply = mail_reply};
    af_open(&f, 3, 8, 4);
    af_create(&f, "MAIL_ROOT");
    af_run(&f);
    ah_error error = {0};
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "queued", "QUEUED_ONLY", false, &error),
          &error);
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "queued", "QUEUED_ONLY", false, &error),
          &error);
    assert(ah_agent_send(f.engine, f.root, f.ids[0], "queued", "CONFLICT", false, &error) ==
           AH_INVALID);
    af_ok(ah_agent_send(f.engine, f.ids[1], f.ids[0], "sibling", "SIBLING_DATA", false, &error),
          &error);
    unsigned calls = f.count;
    af_run(&f);
    assert(f.count == calls);
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "followup", "FOLLOWUP_WAKE", true, &error),
          &error);
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "followup", "FOLLOWUP_WAKE", true, &error),
          &error);
    af_run(&f);
    assert(f.count == calls + 2 && af_session_calls(&f, f.ids[0]) == 2 &&
           af_session_calls(&f, f.ids[1]) == 1);
    af_run(&f);
    assert(f.count == calls + 2);
    char unrelated[AH_ID_CAP];
    af_ok(ah_session_create(f.engine, "mock", "other root", unrelated, &error), &error);
    assert(ah_agent_send(f.engine, unrelated, f.ids[0], "cross", "SECRET", true, &error) != AH_OK);
    assert(ah_agent_send(f.engine, f.root, unrelated, "cross", "SECRET", true, &error) != AH_OK);
    assert(ah_agent_interrupt(f.engine, unrelated, f.ids[0], &error) != AH_OK);
    assert(f.count == calls + 2);
    af_close(&f);
}

static char *stop_reply(agent_fixture *f, agent_request *r) {
    ah_error error = {0};
    assert(ah_agent_send(f->engine, f->root, f->root, "reentry", "data", false, &error) == AH_BUSY);
    if (strcmp(r->session, f->root)) {
        r->hold = true;
        return af_text("STALE_COMPLETION");
    }
    if (r->ordinal == 1)
        return af_call("spawn", "stop_spawn", "{\"name\":\"target\",\"message\":\"TASK_STOP\"}");
    af_spawn_id(r->request, "stop_spawn", f->ids[0]);
    if (r->ordinal == 2) {
        char args[80];
        snprintf(args, sizeof(args), "{\"target\":\"%s\"}", f->ids[0]);
        return af_call("stop", "stop_target", args);
    }
    yyjson_doc *doc = af_receipt(r->request, "stop_target");
    af_result(doc);
    yyjson_doc_free(doc);
    assert(!strstr(r->request, "STALE_COMPLETION"));
    return af_text("STOP_CONFIRMED");
}

static void interrupted_event(agent_fixture *f, const ah_event *event) {
    assert(strcmp(event->type, "model.completed") || !strstr(event->json, "STALE_COMPLETION"));
    if (!strcmp(event->type, "agent.interrupted")) {
        f->interrupted = true;
        for (unsigned i = 0; i < f->count; i++)
            if (strcmp(f->requests[i].session, f->root)) {
                f->requests[i].hold = false;
                f->requests[i].ready = f->tick;
            }
    }
}

static void stop_fences_completion(void) {
    agent_fixture f = {.reply = stop_reply, .on_event = interrupted_event, .latency = 3};
    af_open(&f, 2, 8, 4);
    af_create(&f, "STOP_ROOT");
    af_run(&f);
    assert(f.interrupted && f.child_started == 1 && f.completed == af_session_calls(&f, f.root));
    yyjson_doc *doc = af_agents(&f);
    yyjson_val *usage = yyjson_obj_get(yyjson_doc_get_root(doc), "usage");
    assert(yyjson_get_uint(yyjson_obj_get(usage, "attempts")) == f.count);
    assert(yyjson_get_uint(yyjson_obj_get(usage, "unreported_attempts")) == f.count);
    assert(yyjson_get_uint(yyjson_obj_get(usage, "reported_responses")) == 0);
    yyjson_doc_free(doc);
    af_state(&f, f.ids[0], "stopped");
    af_state(&f, f.root, "completed");
    af_close(&f);
}

static char *limits_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        if (r->ordinal == 1) {
            const char *names[] = {"spawn", "join"}, *ids[] = {"too_deep", "ancestor_join"};
            char args[128];
            snprintf(args, sizeof(args), "{\"ids\":[\"%s\"],\"timeout_ms\":0}", f->root);
            const char *arguments[] = {"{\"name\":\"deep\",\"message\":\"TASK_DEEP\"}", args};
            return af_calls(2, names, ids, arguments);
        }
        error_receipt(r->request, "too_deep");
        error_receipt(r->request, "ancestor_join");
        return af_text("CHILD_LIMITS_CHECKED");
    }
    if (r->ordinal == 1) {
        const char *names[] = {"spawn", "spawn", "spawn", "spawn", "spawn"};
        const char *ids[] = {"nul_name", "limit_a", "limit_b", "too_many", "fork_attempt"};
        const char *args[] = {"{\"name\":\"nul\\u0000tail\",\"message\":\"TASK_NUL\"}",
                              "{\"name\":\"a\",\"message\":\"TASK_A\"}",
                              "{\"name\":\"b\",\"message\":\"TASK_B\"}",
                              "{\"name\":\"c\",\"message\":\"TASK_C\"}",
                              "{\"name\":\"fork\",\"message\":\"TASK_FORK\",\"fork_parent\":true}"};
        return af_calls(5, names, ids, args);
    }
    error_receipt(r->request, "nul_name");
    error_receipt(r->request, "too_many");
    error_receipt(r->request, "fork_attempt");
    af_spawn_id(r->request, "limit_a", f->ids[0]);
    af_spawn_id(r->request, "limit_b", f->ids[1]);
    yyjson_doc *receipt = af_receipt(r->request, "limited_join");
    if (!receipt) {
        const char *ids[] = {f->ids[0], f->ids[1]};
        return af_join("limited_join", ids, 2, 10000);
    }
    yyjson_doc_free(receipt);
    valid_join(r->request, "limited_join", 2);
    return af_text("LIMITS_CHECKED");
}

static void bounds_and_schema(void) {
    agent_fixture f = {.reply = limits_reply};
    af_open(&f, 3, 2, 1);
    af_create(&f, "LIMITS_ROOT");
    af_run(&f);
    assert(f.spawned == 2);
    yyjson_doc *doc = af_agents(&f);
    assert(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "agents")) == 3);
    yyjson_doc_free(doc);
    af_close(&f);
}

static char *usage_reply(agent_fixture *f, agent_request *r) {
    char *body = nested_reply(f, r);
    const char *usage =
        "{\"usage\":{\"input_tokens\":10,\"input_tokens_details\":{\"cached_tokens\":3},"
        "\"output_tokens\":4,\"output_tokens_details\":{\"reasoning_tokens\":1}},";
    size_t size = strlen(body) + strlen(usage);
    char *response = malloc(size);
    assert(response && body[0] == '{');
    assert(snprintf(response, size, "%s%s", usage, body + 1) > 0);
    free(body);
    return response;
}

static void usage_matches_tree_attempts(void) {
    agent_fixture f = {.reply = usage_reply};
    af_open(&f, 2, 8, 4);
    af_create(&f, "USAGE_ROOT");
    af_run(&f);
    yyjson_doc *doc = af_agents(&f);
    yyjson_val *root = yyjson_doc_get_root(doc), *usage = yyjson_obj_get(root, "usage");
    assert(yyjson_get_uint(yyjson_obj_get(usage, "attempts")) == f.count);
    assert(yyjson_get_uint(yyjson_obj_get(usage, "reported_responses")) == f.count);
    assert(yyjson_get_uint(yyjson_obj_get(usage, "unreported_attempts")) == 0);
    const char *keys[] = {"input_tokens", "cached_input_tokens", "output_tokens",
                          "reasoning_tokens"};
    const unsigned per_response[] = {10, 3, 4, 1};
    for (unsigned k = 0; k < 4; k++)
        assert(yyjson_get_uint(yyjson_obj_get(usage, keys[k])) == f.count * per_response[k]);
    yyjson_val *agents = yyjson_obj_get(root, "agents"), *agent;
    size_t index, count;
    unsigned attempts = 0;
    yyjson_arr_foreach(agents, index, count, agent) {
        const char *id = yyjson_get_str(yyjson_obj_get(agent, "id"));
        unsigned calls = af_session_calls(&f, id);
        usage = yyjson_obj_get(agent, "usage");
        assert(yyjson_get_uint(yyjson_obj_get(usage, "attempts")) == calls);
        attempts += calls;
    }
    assert(attempts == f.count);
    yyjson_doc_free(doc);
    af_close(&f);
}

static void cancel_after_spawn(agent_fixture *f, const ah_event *event) {
    if (!f->interrupted && f->spawned == 2 && !strcmp(event->type, "agent.spawned")) {
        f->interrupted = true;
        ah_cancel(f->engine);
    }
}
static char *cancellation_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root))
        return af_text("RESUMED_CHILD");
    if (r->ordinal == 1) {
        const char *names[] = {"spawn", "spawn", "spawn"},
                   *ids[] = {"cancel_a", "cancel_b", "cancel_c"};
        const char *args[] = {"{\"name\":\"a\",\"message\":\"TASK_CANCEL_A\"}",
                              "{\"name\":\"b\",\"message\":\"TASK_CANCEL_B\"}",
                              "{\"name\":\"c\",\"message\":\"TASK_CANCEL_C\"}"};
        return af_calls(3, names, ids, args);
    }
    af_spawn_id(r->request, "cancel_a", f->ids[0]);
    af_spawn_id(r->request, "cancel_b", f->ids[1]);
    af_spawn_id(r->request, "cancel_c", f->ids[2]);
    return af_text("RESUMED_ROOT");
}
static void cancellation_stops_later_dispatch(void) {
    agent_fixture f = {.reply = cancellation_reply, .on_event = cancel_after_spawn};
    af_open(&f, 2, 8, 4);
    af_create(&f, "CANCEL_ROOT");
    ah_error error = {0};
    assert(ah_run(f.engine, f.root, &error) == AH_CANCELLED);
    assert(f.interrupted && f.spawned == 2 && f.count == 1 && !f.active);
    af_state(&f, f.root, "stopped");
    af_ok(ah_resume(f.engine, f.root, &error), &error);
    af_run(&f);
    assert(f.spawned == 3 && f.child_started == 3);
    af_state(&f, f.ids[0], "completed");
    af_state(&f, f.ids[1], "completed");
    af_state(&f, f.ids[2], "completed");
    af_close(&f);
}

int main(int argc, char **argv) {
    if (argc > 1)
        return agent_recovery_child(argc, argv);
    concurrent_fresh_children();
    nested_join_with_one_slot();
    durable_mail_and_boundaries();
    stop_fences_completion();
    bounds_and_schema();
    usage_matches_tree_attempts();
    cancellation_stops_later_dispatch();
    agent_boundary_cases();
    agent_yield_cases();
    agent_transaction_cases();
    agent_recovery_cases(argv[0]);
    puts("agent scheduling, isolation, messaging, and recovery tests passed");
    return 0;
}
