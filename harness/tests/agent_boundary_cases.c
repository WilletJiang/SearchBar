#include "agent_fixture.h"

static void assert_error(const char *request, const char *call) {
    yyjson_doc *doc = af_receipt(request, call);
    assert(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "is_error")));
    yyjson_doc_free(doc);
}

static void capture_operations(agent_fixture *f, const ah_event *event) {
    if (strcmp(event->type, "tool.intent"))
        return;
    yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
    assert(doc && event->operation_id);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_equals_str(yyjson_obj_get(root, "name"), "private_value"))
        memcpy(f->ids[!strcmp(event->session_id, f->root) ? 3 : 4], event->operation_id, AH_ID_CAP);
    if (yyjson_equals_str(yyjson_obj_get(root, "name"), "ask"))
        memcpy(f->ids[2], event->operation_id, AH_ID_CAP);
    yyjson_doc_free(doc);
}

static ah_code private_value(void *context, const ah_tool_call *call, ah_tool_result *result,
                             ah_error *error) {
    (void)error;
    agent_fixture *f = context;
    assert(pthread_equal(f->owner, pthread_self()));
    result->json =
        strdup(!strcmp(call->session_id, f->root) ? "{\"secret\":\"PARENT_TOOL_SECRET\"}"
                                                  : "{\"secret\":\"CHILD_TOOL_SECRET\"}");
    assert(result->json);
    return AH_OK;
}

static char *private_reply(agent_fixture *f, agent_request *r) {
    bool root = !strcmp(r->session, f->root);
    if (!root)
        assert(!strstr(r->request, "PARENT_TOOL_SECRET"));
    if (!root && strstr(r->request, "TASK_OWNER")) {
        if (r->ordinal == 1)
            return af_call("private_value", "owner_value", "{}");
        if (strstr(r->request, "INSPECT_MAIL"))
            assert(af_occurrences(r->request, "BUILTIN_SIBLING") == 1);
        return af_text("OWNER_DONE");
    }
    if (!root) {
        assert(!strstr(r->request, "CHILD_TOOL_SECRET"));
        if (!strstr(r->request, "READ_SIBLING"))
            return af_text("RECEIVER_DONE");
        yyjson_doc *doc = af_receipt(r->request, "read_sibling");
        if (!doc) {
            const char *names[] = {"read_result", "read_result", "read_result", "join", "send"};
            const char *ids[] = {"read_parent", "read_sibling", "read_final_sibling",
                                 "join_sibling", "send_sibling"};
            char parent[96], sibling[96], final[96], join[128], send[160];
            assert(*f->ids[3] && *f->ids[4] && *f->ids[6]);
            snprintf(parent, sizeof(parent), "{\"operation_id\":\"%s\"}", f->ids[3]);
            snprintf(sibling, sizeof(sibling), "{\"operation_id\":\"%s\"}", f->ids[4]);
            snprintf(final, sizeof(final), "{\"operation_id\":\"%s\"}", f->ids[6]);
            snprintf(join, sizeof(join), "{\"ids\":[\"%s\"],\"timeout_ms\":0}", f->ids[0]);
            snprintf(send, sizeof(send),
                     "{\"target\":\"%s\",\"message\":\"BUILTIN_SIBLING\",\"wake\":false}",
                     f->ids[0]);
            const char *arguments[] = {parent, sibling, final, join, send};
            return af_calls(5, names, ids, arguments);
        }
        yyjson_doc_free(doc);
        assert_error(r->request, "read_parent");
        assert_error(r->request, "read_sibling");
        assert_error(r->request, "read_final_sibling");
        assert_error(r->request, "join_sibling");
        doc = af_receipt(r->request, "send_sibling");
        af_result(doc);
        yyjson_doc_free(doc);
        return af_text("BOUNDARIES_CHECKED");
    }
    if (r->ordinal == 1) {
        const char *names[] = {"private_value", "spawn", "spawn"};
        const char *ids[] = {"root_value", "owner", "receiver"};
        const char *args[] = {"{}", "{\"name\":\"owner\",\"message\":\"TASK_OWNER\"}",
                              "{\"name\":\"receiver\",\"message\":\"TASK_RECEIVER\"}"};
        return af_calls(3, names, ids, args);
    }
    af_spawn_id(r->request, "owner", f->ids[0]);
    af_spawn_id(r->request, "receiver", f->ids[1]);
    yyjson_doc *doc = af_receipt(r->request, "private_join");
    if (!doc) {
        const char *ids[] = {f->ids[0], f->ids[1]};
        return af_join("private_join", ids, 2, 10000);
    }
    yyjson_doc_free(doc);
    doc = af_receipt(r->request, "child_final_result");
    if (!doc) {
        char args[96];
        af_terminal_operation(r->request, f->ids[0], f->ids[6]);
        snprintf(args, sizeof(args), "{\"operation_id\":\"%s\"}", f->ids[6]);
        return af_call("read_result", "child_final_result", args);
    }
    const char *full_result = yyjson_get_str(yyjson_obj_get(af_result(doc), "content"));
    assert(full_result && strstr(full_result, "OWNER_DONE"));
    assert(!strstr(full_result, "CHILD_TOOL_SECRET"));
    yyjson_doc_free(doc);
    doc = af_receipt(r->request, "tree_view");
    if (!doc)
        return af_call("agents", "tree_view", "{}");
    yyjson_val *view = af_result(doc);
    assert(yyjson_arr_size(yyjson_obj_get(view, "agents")) == 3);
    assert(!strstr(r->request, f->ids[5]));
    yyjson_doc_free(doc);
    return af_text("ROOT_DONE");
}

static void result_privacy_and_sibling_tools(void) {
    agent_fixture f = {.reply = private_reply, .on_event = capture_operations};
    af_open(&f, 3, 8, 4);
    ah_error error = {0};
    ah_tool tool = {.name = "private_value",
                    .description = "Return a private session result",
                    .parameters_json = "{\"type\":\"object\",\"additionalProperties\":false}",
                    .effect = AH_EFFECT_READ,
                    .execute = private_value,
                    .context = &f};
    af_ok(ah_register_tool(f.engine, &tool, &error), &error);
    af_ok(ah_session_create(f.engine, "mock", "UNRELATED", f.ids[5], &error), &error);
    af_create(&f, "PRIVATE_ROOT");
    af_run(&f);
    unsigned owner_calls = af_session_calls(&f, f.ids[0]);
    af_ok(
        ah_agent_send(f.engine, f.root, f.ids[1], "read-boundaries", "READ_SIBLING", true, &error),
        &error);
    af_run(&f);
    assert(af_session_calls(&f, f.ids[0]) == owner_calls);
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "read-mail", "INSPECT_MAIL", true, &error),
          &error);
    af_run(&f);
    assert(af_session_calls(&f, f.ids[0]) == owner_calls + 1);
    af_close(&f);
}

static char *paused_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        r->ready = f->tick + 10;
        memcpy(f->ids[0], r->session, AH_ID_CAP);
        return af_text("CHILD_FINISHED_WHILE_PARENT_WAITED");
    }
    if (r->ordinal == 1) {
        const char *names[] = {"spawn", "ask"}, *ids[] = {"paused_spawn", "root_question"};
        const char *args[] = {"{\"name\":\"worker\",\"message\":\"TASK_WHILE_WAITING\"}",
                              "{\"question\":\"Continue?\"}"};
        return af_calls(2, names, ids, args);
    }
    assert(af_occurrences(r->request, "CHILD_FINISHED_WHILE_PARENT_WAITED") == 1);
    return af_text("PARENT_RESUMED");
}

static void paused_root_does_not_block_children(void) {
    agent_fixture f = {.reply = paused_reply, .on_event = capture_operations};
    af_open(&f, 1, 8, 4);
    af_create(&f, "PAUSED_ROOT");
    af_run(&f);
    assert(f.count == 2 && *f.ids[0] && *f.ids[2]);
    af_state(&f, f.root, "waiting_input");
    af_state(&f, f.ids[0], "completed");
    af_run(&f);
    assert(f.count == 2);
    ah_error error = {0};
    af_ok(ah_reply(f.engine, f.root, f.ids[2], "{\"continue\":true}", &error), &error);
    af_run(&f);
    assert(f.count == 3);
    af_state(&f, f.root, "completed");
    af_close(&f);
}

static ah_code uncertain_effect(void *context, const ah_tool_call *call, ah_tool_result *result,
                                ah_error *error) {
    (void)context;
    (void)call;
    (void)error;
    result->outcome_unknown = true;
    result->json = strdup("{\"effect_may_have_committed\":true}");
    assert(result->json);
    return AH_OK;
}

static char *uncertain_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        assert(r->ordinal == 1);
        return af_call("uncertain_effect", "uncertain_child", "{}");
    }
    if (r->ordinal == 1)
        return af_call("spawn", "unknown_spawn",
                       "{\"name\":\"uncertain\",\"message\":\"TASK_UNKNOWN\"}");
    af_spawn_id(r->request, "unknown_spawn", f->ids[0]);
    yyjson_doc *doc = af_receipt(r->request, "unknown_join");
    if (!doc) {
        const char *ids[] = {f->ids[0]};
        return af_join("unknown_join", ids, 1, 10000);
    }
    yyjson_val *agents = yyjson_obj_get(af_result(doc), "agents");
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_arr_get_first(agents), "state"),
                             "outcome_unknown"));
    yyjson_doc_free(doc);
    return af_text("RECONCILIATION_REQUIRED");
}

static void wake_does_not_repeat_uncertain_effect(void) {
    agent_fixture f = {.reply = uncertain_reply};
    af_open(&f, 2, 8, 4);
    ah_error error = {0};
    ah_tool tool = {.name = "uncertain_effect",
                    .description = "Ambiguous effect result",
                    .parameters_json = "{\"type\":\"object\",\"additionalProperties\":false}",
                    .effect = AH_EFFECT_WRITE,
                    .execute = uncertain_effect};
    af_ok(ah_register_tool(f.engine, &tool, &error), &error);
    af_create(&f, "UNKNOWN_ROOT");
    af_run(&f);
    af_state(&f, f.ids[0], "outcome_unknown");
    unsigned requests = f.count;
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "retry", "WAKE_UNCERTAIN", true, &error),
          &error);
    af_run(&f);
    assert(f.count == requests);
    assert(ah_resume(f.engine, f.ids[0], &error) == AH_UNCERTAIN);
    af_state(&f, f.ids[0], "outcome_unknown");
    af_close(&f);
}

static void steer_on_boundary(agent_fixture *f, const ah_event *event) {
    const char *state = f->phase == 1 ? "completed" : "waiting_agents";
    if (f->interrupted || strcmp(event->session_id, f->root) || strcmp(event->type, "state") ||
        !strstr(event->json, state))
        return;
    f->interrupted = true;
    ah_error error = {0};
    af_ok(ah_steer(f->engine, f->root, "late-command", "ROOT_STEER_AFTER_BOUNDARY", &error),
          &error);
}

static char *steering_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        r->ready = f->tick + 40;
        return af_text("STEERING_CHILD_DONE");
    }
    if (r->ordinal == 1)
        return af_call("spawn", "steer_spawn", "{\"name\":\"worker\",\"message\":\"TASK_STEER\"}");
    af_spawn_id(r->request, "steer_spawn", f->ids[0]);
    if (f->phase == 2) {
        yyjson_doc *doc = af_receipt(r->request, "steered_join");
        if (!doc) {
            const char *ids[] = {f->ids[0]};
            return af_join("steered_join", ids, 1, 10000);
        }
        yyjson_val *result = af_result(doc);
        assert(yyjson_is_true(yyjson_obj_get(result, "interrupted")));
        assert(yyjson_is_false(yyjson_obj_get(result, "timed_out")));
        yyjson_doc_free(doc);
    }
    if (r->ordinal >= 3)
        assert(af_occurrences(r->request, "ROOT_STEER_AFTER_BOUNDARY") == 1);
    return af_text("STEERING_ROOT_DONE");
}

static void steering_reaches_root_boundaries(void) {
    for (unsigned phase = 1; phase <= 2; phase++) {
        agent_fixture f = {.reply = steering_reply, .on_event = steer_on_boundary, .phase = phase};
        af_open(&f, 2, 8, 4);
        af_create(&f, "STEER_ROOT");
        af_run(&f);
        assert(f.interrupted && af_session_calls(&f, f.root) >= 3);
        af_state(&f, f.root, "completed");
        af_state(&f, f.ids[0], "completed");
        af_close(&f);
    }
}

static char *wait_any_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        bool first = strstr(r->request, "TASK_FIRST") != NULL;
        r->ready = f->tick + (first ? 10 : 40);
        return af_text(first ? "FIRST_FINISHED" : "SECOND_FINISHED");
    }
    if (r->ordinal == 1) {
        const char *names[] = {"spawn", "spawn"}, *ids[] = {"first", "second"};
        const char *args[] = {"{\"name\":\"first\",\"message\":\"TASK_FIRST\"}",
                              "{\"name\":\"second\",\"message\":\"TASK_SECOND\"}"};
        return af_calls(2, names, ids, args);
    }
    af_spawn_id(r->request, "first", f->ids[0]);
    af_spawn_id(r->request, "second", f->ids[1]);
    if (r->ordinal == 2) {
        char args[256];
        snprintf(args, sizeof(args),
                 "{\"ids\":[\"%s\",\"%s\"],\"mode\":\"any\",\"timeout_ms\":10000}", f->ids[0],
                 f->ids[1]);
        return af_call("join", "first_ready", args);
    }
    yyjson_doc *doc = af_receipt(r->request, "first_ready");
    yyjson_val *result = af_result(doc), *states = yyjson_obj_get(result, "agents");
    assert(yyjson_is_false(yyjson_obj_get(result, "timed_out")));
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_arr_get(states, 0), "state"), "completed"));
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_arr_get(states, 1), "state"), "running"));
    yyjson_doc_free(doc);
    if (r->ordinal == 3) {
        const char *ids[] = {f->ids[1]};
        return af_join("remaining", ids, 1, 10000);
    }
    doc = af_receipt(r->request, "remaining");
    af_result(doc);
    yyjson_doc_free(doc);
    assert(af_occurrences(r->request, "FIRST_FINISHED") == 1);
    assert(af_occurrences(r->request, "SECOND_FINISHED") == 1);
    return af_text("WAIT_ANY_CHECKED");
}

static void explicit_wait_any_returns_early(void) {
    agent_fixture f = {.reply = wait_any_reply};
    af_open(&f, 3, 8, 4);
    af_create(&f, "WAIT_ANY_ROOT");
    af_run(&f);
    assert(af_session_calls(&f, f.root) == 4 && f.child_started == 2);
    af_close(&f);
}

static char *active_mail_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        if (r->ordinal == 1) {
            r->ready = f->tick + 30;
            return af_text("PRELIMINARY_CHILD_RESULT");
        }
        assert(r->ordinal == 2 && af_occurrences(r->request, "ACTIVE_QUEUE_ONLY_INFO") == 1);
        return af_text("FINAL_CHILD_ACKNOWLEDGMENT");
    }
    if (r->ordinal == 1)
        return af_call("spawn", "active_spawn",
                       "{\"name\":\"worker\",\"message\":\"TASK_ACTIVE_MAIL\"}");
    af_spawn_id(r->request, "active_spawn", f->ids[0]);
    if (r->ordinal == 2) {
        char args[192];
        snprintf(args, sizeof(args),
                 "{\"target\":\"%s\",\"message\":\"ACTIVE_QUEUE_ONLY_INFO\",\"wake\":false}",
                 f->ids[0]);
        return af_call("send", "active_send", args);
    }
    yyjson_doc *doc = af_receipt(r->request, "active_join");
    if (!doc) {
        const char *ids[] = {f->ids[0]};
        return af_join("active_join", ids, 1, 10000);
    }
    af_result(doc);
    yyjson_doc_free(doc);
    assert(af_occurrences(r->request, "FINAL_CHILD_ACKNOWLEDGMENT") == 1);
    assert(!strstr(r->request, "PRELIMINARY_CHILD_RESULT"));
    return af_text("ACTIVE_MAIL_CHECKED");
}

static void queued_active_mail_reaches_next_model_boundary(void) {
    agent_fixture f = {.reply = active_mail_reply};
    af_open(&f, 2, 8, 4);
    af_create(&f, "ACTIVE_MAIL_ROOT");
    af_run(&f);
    assert(af_session_calls(&f, f.ids[0]) == 2 && af_session_calls(&f, f.root) == 4);
    yyjson_doc *doc = af_agents(&f);
    yyjson_val *agents = yyjson_obj_get(yyjson_doc_get_root(doc), "agents");
    yyjson_val *child = yyjson_arr_get(agents, 1);
    assert(yyjson_get_uint(yyjson_obj_get(child, "turn")) == 1);
    yyjson_doc_free(doc);
    af_close(&f);
}

void agent_boundary_cases(void) {
    result_privacy_and_sibling_tools();
    paused_root_does_not_block_children();
    wake_does_not_repeat_uncertain_effect();
    steering_reaches_root_boundaries();
    explicit_wait_any_returns_early();
    queued_active_mail_reaches_next_model_boundary();
}
