#include "agent_fixture.h"
#include "agents/agents_internal.h"
#include <unistd.h>

static const char task_policy[] =
    "1111111111111111111111111111111111111111111111111111111111111111";

/* Internal access builds deterministic SQLite fault/migration fixtures and
 * inspects the durable rows. Normal scheduling and retries use public APIs. */
static void sql(ah_engine *engine, const char *statement) {
    ah_error error = {0};
    af_ok(core_sql(engine, statement, &error), &error);
}

static sqlite3_int64 scalar(ah_engine *engine, const char *statement) {
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(engine->db, statement, -1, &query, NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW);
    sqlite3_int64 value = sqlite3_column_int64(query, 0);
    sqlite3_finalize(query);
    return value;
}

static ah_code task_check(void *context, const ah_tool_call *call, ah_tool_result *result,
                          ah_error *error) {
    (void)error;
    agent_fixture *fixture = context;
    assert(strcmp(call->session_id, fixture->root));
    assert(call->deadline_ms > ah_now_ms() && call->deadline_ms <= ah_now_ms() + 10000);
    fixture->phase++;
    result->json = strdup("{\"checked\":true}");
    assert(result->json);
    return AH_OK;
}

static char *task_reply(agent_fixture *fixture, agent_request *request) {
    if (strcmp(request->session, fixture->root))
        return request->ordinal == 1 ? af_call("transaction_check", "child_check", "{}")
                                     : af_text("CHILD_CHECKED");
    if (request->ordinal == 1)
        return af_call("spawn", "transaction_spawn",
                       "{\"name\":\"atomic\",\"message\":\"CHECK_CHILD\"}");
    af_spawn_id(request->request, "transaction_spawn", fixture->ids[0]);
    yyjson_doc *joined = af_receipt(request->request, "transaction_join");
    if (!joined) {
        const char *ids[] = {fixture->ids[0]};
        return af_join("transaction_join", ids, 1, 10000);
    }
    yyjson_doc_free(joined);
    return af_text("TASK_DONE");
}

static void spawn_rollback_and_task_budget(void) {
    agent_fixture fixture = {.reply = task_reply};
    af_open(&fixture, 2, 4, 2);
    af_create(&fixture, "TRANSACTION_ROOT");
    ah_error error = {0};
    ah_task_config task = {.contract_json = "{\"required\":\"check child\"}",
                           .verifier_revision = task_policy,
                           .max_model_calls = 5,
                           .max_tool_calls = 3,
                           .timeout_ms = 10000};
    af_ok(ah_task_create(fixture.engine, fixture.root, &task, &error), &error);
    ah_tool check = {.name = "transaction_check",
                     .description = "Check the inherited task deadline.",
                     .parameters_json = "{\"type\":\"object\",\"properties\":{},"
                                        "\"additionalProperties\":false}",
                     .effect = AH_EFFECT_READ,
                     .execute = task_check,
                     .context = &fixture};
    af_ok(ah_register_tool(fixture.engine, &check, &error), &error);
    sql(fixture.engine,
        "CREATE TEMP TRIGGER fail_spawn BEFORE INSERT ON events "
        "WHEN NEW.type='tool.completed' BEGIN SELECT RAISE(ROLLBACK,'injected spawn'); END");
    assert(ah_run(fixture.engine, fixture.root, &error) == AH_INVALID);
    assert(!fixture.active && fixture.spawned == 0 && fixture.child_started == 0);
    assert(scalar(fixture.engine, "SELECT COUNT(*) FROM sessions") == 1);
    assert(scalar(fixture.engine, "SELECT COUNT(*) FROM agent_nodes") == 1);
    assert(scalar(fixture.engine, "SELECT COUNT(*) FROM agent_messages") == 0);
    assert(scalar(fixture.engine, "SELECT COUNT(*) FROM operations WHERE status='pending'") == 1);
    assert(scalar(fixture.engine, "SELECT tool_used FROM agent_trees") == 0);
    assert(scalar(fixture.engine, "SELECT tool_used FROM tasks") == 0);
    assert(scalar(fixture.engine, "SELECT model_used FROM agent_trees") == 1);
    assert(scalar(fixture.engine, "SELECT model_used FROM tasks") == 1);
    sql(fixture.engine, "DROP TRIGGER fail_spawn");
    af_ok(ah_resume(fixture.engine, fixture.root, &error), &error);
    af_run(&fixture);
    assert(fixture.spawned == 1 && fixture.child_started == 2 && fixture.phase == 1);
    assert(fixture.count == 5 && fixture.receipts == 3);
    yyjson_doc *tree = af_agents(&fixture);
    yyjson_val *root = yyjson_doc_get_root(tree);
    assert(yyjson_arr_size(yyjson_obj_get(root, "agents")) == 2);
    assert(yyjson_get_uint(yyjson_obj_get(root, "model_used")) == 5);
    assert(yyjson_get_uint(yyjson_obj_get(root, "tool_used")) == 3);
    yyjson_doc_free(tree);
    char *json = NULL;
    af_ok(ah_task_json(fixture.engine, fixture.root, &json, &error), &error);
    yyjson_doc *document = yyjson_read(json, strlen(json), 0);
    root = yyjson_doc_get_root(document);
    assert(yyjson_get_uint(yyjson_obj_get(root, "model_used")) == 5);
    assert(yyjson_get_uint(yyjson_obj_get(root, "tool_used")) == 3);
    yyjson_doc_free(document);
    free(json);
    af_ok(ah_submit(fixture.engine, fixture.root, "budget_retry", "Another turn", &error), &error);
    assert(ah_run(fixture.engine, fixture.root, &error) == AH_LIMIT && fixture.count == 5);
    af_close(&fixture);
}

static char *mail_reply(agent_fixture *fixture, agent_request *request) {
    if (strcmp(request->session, fixture->root)) {
        if (request->ordinal > 1) {
            assert(af_occurrences(request->request, "QUEUE_ONCE") == 1);
            assert(af_occurrences(request->request, "DELIVER_ONCE") == 1);
        }
        return af_text("MAIL_DONE");
    }
    if (request->ordinal == 1)
        return af_call("spawn", "mail_spawn", "{\"name\":\"mail\",\"message\":\"INITIAL_MAIL\"}");
    af_spawn_id(request->request, "mail_spawn", fixture->ids[0]);
    return af_text("ROOT_DONE");
}

static void mailbox_rollbacks(void) {
    agent_fixture fixture = {.reply = mail_reply};
    af_open(&fixture, 2, 4, 2);
    af_create(&fixture, "MAIL_ROLLBACK_ROOT");
    af_run(&fixture);
    ah_error error = {0};
    sql(fixture.engine,
        "CREATE TEMP TRIGGER fail_mail BEFORE INSERT ON events "
        "WHEN NEW.type='agent.message_queued' BEGIN SELECT RAISE(ROLLBACK,'injected mail'); END");
    assert(ah_agent_send(fixture.engine, fixture.root, fixture.ids[0], "tx-queued", "QUEUE_ONCE",
                         false, &error) == AH_INVALID);
    assert(scalar(fixture.engine,
                  "SELECT COUNT(*) FROM agent_messages WHERE message_id='tx-queued'") == 0);
    sql(fixture.engine, "DROP TRIGGER fail_mail");
    for (unsigned i = 0; i < 2; i++)
        af_ok(ah_agent_send(fixture.engine, fixture.root, fixture.ids[0], "tx-queued", "QUEUE_ONCE",
                            false, &error),
              &error);
    af_ok(ah_agent_send(fixture.engine, fixture.root, fixture.ids[0], "tx-wake", "DELIVER_ONCE",
                        true, &error),
          &error);
    sql(fixture.engine,
        "CREATE TEMP TRIGGER fail_delivery BEFORE UPDATE OF consumed ON agent_messages "
        "WHEN NEW.consumed=1 AND OLD.message_id='tx-wake' "
        "BEGIN SELECT RAISE(ROLLBACK,'injected delivery'); END");
    core_session child;
    af_ok(core_load(fixture.engine, fixture.ids[0], &child, &error), &error);
    unsigned turn = child.turn;
    bool changed = true;
    assert(agents_drain(fixture.engine, &child, &changed, &error) == AH_INVALID && !changed);
    assert(child.turn == turn && child.state == AH_COMPLETED);
    core_session_free(&child);
    assert(scalar(fixture.engine,
                  "SELECT COUNT(*) FROM agent_messages WHERE message_id IN ('tx-queued','tx-wake') "
                  "AND consumed=0") == 2);
    assert(scalar(fixture.engine, "SELECT COUNT(*) FROM items WHERE json LIKE '%QUEUE_ONCE%' "
                                  "OR json LIKE '%DELIVER_ONCE%'") == 0);
    assert(scalar(fixture.engine, "SELECT COUNT(*) FROM events WHERE type='agent.message_received' "
                                  "AND (json LIKE '%tx-queued%' OR json LIKE '%tx-wake%')") == 0);
    /* The initial idle-mail sweep must surface a persistent storage fault and
     * stop the tree, rather than retrying the old wake forever in one run. */
    assert(ah_run(fixture.engine, fixture.root, &error) == AH_INVALID);
    af_state(&fixture, fixture.root, "stopped");
    unsigned failed_requests = fixture.count;
    assert(ah_run(fixture.engine, fixture.root, &error) == AH_INVALID);
    assert(fixture.count == failed_requests && fixture.child_started == 1);
    sql(fixture.engine, "DROP TRIGGER fail_delivery");
    af_ok(ah_resume(fixture.engine, fixture.root, &error), &error);
    af_run(&fixture);
    assert(fixture.child_started == 2);
    assert(scalar(fixture.engine,
                  "SELECT COUNT(*) FROM agent_messages WHERE message_id IN ('tx-queued','tx-wake') "
                  "AND consumed=1") == 2);
    assert(scalar(fixture.engine, "SELECT COUNT(*) FROM items WHERE json LIKE '%QUEUE_ONCE%' "
                                  "OR json LIKE '%DELIVER_ONCE%'") == 2);
    assert(scalar(fixture.engine, "SELECT COUNT(*) FROM events WHERE type='agent.message_received' "
                                  "AND (json LIKE '%tx-queued%' OR json LIKE '%tx-wake%')") == 2);
    unsigned requests = fixture.count;
    af_ok(ah_agent_send(fixture.engine, fixture.root, fixture.ids[0], "tx-wake", "DELIVER_ONCE",
                        true, &error),
          &error);
    af_run(&fixture);
    assert(fixture.count == requests);
    af_close(&fixture);
}

static void old_schema_upgrades(void) {
    const char *versions[] = {"PRAGMA user_version=1", "PRAGMA user_version=2",
                              "PRAGMA user_version=3"};
    for (unsigned version = 0; version < 3; version++) {
        char directory[] = "/tmp/ah-agent-migration-XXXXXX", path[128];
        assert(mkdtemp(directory));
        snprintf(path, sizeof(path), "%s/state.db", directory);
        agent_fixture fixture = {.database = path, .reply = mail_reply};
        af_open(&fixture, 2, 4, 2);
        ah_error error = {0};
        af_ok(ah_session_create(fixture.engine, "mock", "HOST_INSTRUCTIONS_914", fixture.root,
                                &error),
              &error);
        /* Remove every new table, not just the version marker, to exercise
         * actual creation when opening each supported pre-agent schema. */
        sql(fixture.engine,
            "DROP TABLE agent_usage; DROP TABLE agent_waits; DROP TABLE agent_messages; "
            "DROP TABLE agent_trees; DROP TABLE agent_nodes");
        if (version == 0)
            sql(fixture.engine, "DROP TABLE result_refs");
        sql(fixture.engine, versions[version]);
        af_reopen(&fixture);
        assert(scalar(fixture.engine, "PRAGMA user_version") == 4);
        assert(scalar(fixture.engine, "SELECT COUNT(*) FROM agent_nodes") == 0);
        af_state(&fixture, fixture.root, "idle");
        af_ok(ah_submit(fixture.engine, fixture.root, "migration", "MIGRATION_ROOT", &error),
              &error);
        af_run(&fixture);
        assert(fixture.spawned == 1 && fixture.child_started == 1);
        assert(scalar(fixture.engine, "SELECT COUNT(*) FROM pragma_foreign_key_check") == 0);
        af_close(&fixture);
        af_remove_database(path);
        assert(rmdir(directory) == 0);
    }
}

static char *unfinished_child_reply(agent_fixture *fixture, agent_request *request) {
    if (strcmp(request->session, fixture->root)) {
        if (request->ordinal == 1)
            return af_call("ask", "acceptance_question", "{\"question\":\"Required answer?\"}");
        yyjson_doc *reply = af_receipt(request->request, "acceptance_question");
        assert(yyjson_equals_str(yyjson_obj_get(af_result(reply), "answer"), "go"));
        yyjson_doc_free(reply);
        return af_text("CHILD_COMPLETE");
    }
    if (request->ordinal == 1)
        return af_call("spawn", "acceptance_child",
                       "{\"name\":\"acceptance\",\"message\":\"ASK_FIRST\"}");
    af_spawn_id(request->request, "acceptance_child", fixture->ids[0]);
    return af_text("ROOT_FINAL");
}

static ah_code acceptance_revision(void *context, const ah_tool_call *call, char revision[65],
                                   ah_error *error) {
    (void)context;
    (void)call;
    (void)error;
    memcpy(revision, task_policy, sizeof(task_policy));
    return AH_OK;
}

static ah_code acceptance_verify(void *context, const ah_tool_call *call, ah_tool_result *result,
                                 ah_error *error) {
    (void)call;
    (void)error;
    agent_fixture *fixture = context;
    fixture->phase++;
    result->json = strdup("{\"ok\":true}");
    assert(result->json);
    return AH_OK;
}

static void expect_task_state(agent_fixture *fixture, const char *state) {
    char *json = NULL;
    ah_error error = {0};
    af_ok(ah_task_json(fixture->engine, fixture->root, &json, &error), &error);
    yyjson_doc *document = yyjson_read(json, strlen(json), 0);
    assert(document &&
           yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(document), "state"), state));
    yyjson_doc_free(document);
    free(json);
}

static ah_code unexpected_sync(void *context, const char *request, ah_delta_sink delta, void *dc,
                               ah_cancel_check cancelled, void *cc, char **response,
                               ah_error *error) {
    (void)context;
    (void)request;
    (void)delta;
    (void)dc;
    (void)cancelled;
    (void)cc;
    (void)response;
    (void)error;
    assert(!"completed root must not call the synchronous provider");
    return AH_PROTOCOL;
}

static void task_waits_for_child_reply(void) {
    char directory[] = "/tmp/ah-task-child-XXXXXX", path[128];
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/state.db", directory);
    agent_fixture fixture = {.database = path, .reply = unfinished_child_reply};
    af_open(&fixture, 2, 4, 2);
    af_create(&fixture, "TASK_ACCEPTANCE_ROOT");
    ah_error error = {0};
    ah_task_config task = {.contract_json = "{\"required\":\"child answer\"}",
                           .verifier_revision = task_policy,
                           .max_model_calls = 20,
                           .max_tool_calls = 20,
                           .timeout_ms = 10000};
    af_ok(ah_task_create(fixture.engine, fixture.root, &task, &error), &error);
    ah_task_verifier verifier = {task_policy, acceptance_revision, acceptance_verify, &fixture};
    af_ok(ah_task_run(fixture.engine, fixture.root, &verifier, &error), &error);
    assert(fixture.phase == 0);
    af_state(&fixture, fixture.root, "completed");
    af_state(&fixture, fixture.ids[0], "waiting_input");
    expect_task_state(&fixture, "active");
    ah_close(fixture.engine);
    ah_config disabled;
    ah_config_init(&disabled);
    disabled.database_path = path;
    disabled.provider.complete = unexpected_sync;
    af_ok(ah_open(&disabled, &fixture.engine, &error), &error);
    af_ok(ah_task_run(fixture.engine, fixture.root, &verifier, &error), &error);
    assert(fixture.phase == 0);
    expect_task_state(&fixture, "active");
    ah_close(fixture.engine);
    af_open(&fixture, 2, 4, 2);
    char *json = NULL, operation[AH_ID_CAP];
    af_ok(ah_session_json(fixture.engine, fixture.ids[0], &json, &error), &error);
    yyjson_doc *document = yyjson_read(json, strlen(json), 0);
    const char *id = yyjson_get_str(
        yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(document), "pending"), "operation_id"));
    assert(id && strlen(id) == AH_ID_CAP - 1);
    memcpy(operation, id, AH_ID_CAP);
    yyjson_doc_free(document);
    free(json);
    af_ok(ah_reply(fixture.engine, fixture.ids[0], operation, "{\"answer\":\"go\"}", &error),
          &error);
    af_ok(ah_task_run(fixture.engine, fixture.root, &verifier, &error), &error);
    assert(fixture.phase == 1);
    af_state(&fixture, fixture.ids[0], "completed");
    expect_task_state(&fixture, "accepted");
    af_ok(ah_task_run(fixture.engine, fixture.root, &verifier, &error), &error);
    assert(fixture.phase == 1);
    af_close(&fixture);
    af_remove_database(path);
    assert(rmdir(directory) == 0);
}

static void replace_pending_policy(agent_fixture *fixture, const ah_event *event) {
    if (strcmp(event->type, "tool.intent"))
        return;
    const char *changes[] = {
        ("UPDATE operations SET schema='{\"type\":\"object\",\"properties\":{}}' "
         "WHERE name='spawn' AND status='pending'"),
        "UPDATE operations SET effect=1 WHERE name='spawn' AND status='pending'",
        "UPDATE operations SET approval=1 WHERE name='spawn' AND status='pending'"};
    /* Emulate a durable custom-tool intent written before agents were enabled;
     * its historical authorization must not become a coordinator operation. */
    sql(fixture->engine, changes[fixture->phase]);
}

static char *changed_policy_reply(agent_fixture *fixture, agent_request *request) {
    assert(!strcmp(request->session, fixture->root));
    if (request->ordinal == 1)
        return af_call("spawn", "saved_policy",
                       "{\"name\":\"forbidden\",\"message\":\"DO_NOT_START\"}");
    yyjson_doc *receipt = af_receipt(request->request, "saved_policy");
    assert(receipt && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(receipt), "is_error")));
    yyjson_doc_free(receipt);
    return af_text("OLD_POLICY_REJECTED");
}

static void old_custom_intents_do_not_delegate(void) {
    for (unsigned policy = 0; policy < 3; policy++) {
        agent_fixture fixture = {
            .reply = changed_policy_reply, .on_event = replace_pending_policy, .phase = policy};
        af_open(&fixture, 2, 4, 2);
        af_create(&fixture, "SAVED_POLICY_ROOT");
        af_run(&fixture);
        assert(fixture.count == 2 && fixture.receipts == 1 && fixture.spawned == 0);
        yyjson_doc *tree = af_agents(&fixture);
        assert(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(tree), "agents")) == 1);
        yyjson_doc_free(tree);
        af_close(&fixture);
    }
}

void agent_transaction_cases(void) {
    spawn_rollback_and_task_budget();
    mailbox_rollbacks();
    old_schema_upgrades();
    task_waits_for_child_reply();
    old_custom_intents_do_not_delegate();
}
