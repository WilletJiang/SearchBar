#include "agent_fixture.h"
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static char *recovery_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        r->ready = f->tick + 20; /* Parent parks its join before this result becomes available. */
        return af_text("DURABLE_RESULT");
    }
    yyjson_doc *doc = af_receipt(r->request, "durable_spawn");
    if (!doc)
        return af_call("spawn", "durable_spawn",
                       "{\"name\":\"durable\",\"message\":\"TASK_DURABLE\"}");
    yyjson_doc_free(doc);
    af_spawn_id(r->request, "durable_spawn", f->ids[0]);
    doc = af_receipt(r->request, "durable_join");
    if (!doc) {
        const char *ids[] = {f->ids[0]};
        return af_join("durable_join", ids, 1, 10000);
    }
    yyjson_val *result = af_result(doc);
    assert(yyjson_is_false(yyjson_obj_get(result, "timed_out")));
    yyjson_doc_free(doc);
    unsigned delivered = af_occurrences(r->request, "DURABLE_RESULT");
    if (delivered != 1)
        fprintf(stderr, "recovery checkpoint %u delivered %u result previews\n", f->phase,
                delivered);
    assert(delivered == 1);
    return af_text("RECOVERY_DONE");
}

static char *active_recovery_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        if (strstr(r->request, "DURABLE_ACTIVE_INFO"))
            return af_text("FINAL_ACTIVE_ACK");
        r->ready = f->tick + 30;
        return af_text("PRELIM_ACTIVE_RESULT");
    }
    yyjson_doc *doc = af_receipt(r->request, "durable_spawn");
    if (!doc)
        return af_call("spawn", "durable_spawn",
                       "{\"name\":\"active\",\"message\":\"TASK_ACTIVE\"}");
    yyjson_doc_free(doc);
    af_spawn_id(r->request, "durable_spawn", f->ids[0]);
    doc = af_receipt(r->request, "durable_send");
    if (!doc) {
        char args[192];
        snprintf(args, sizeof(args),
                 "{\"target\":\"%s\",\"message\":\"DURABLE_ACTIVE_INFO\",\"wake\":false}",
                 f->ids[0]);
        return af_call("send", "durable_send", args);
    }
    yyjson_doc_free(doc);
    doc = af_receipt(r->request, "durable_join");
    if (!doc) {
        const char *ids[] = {f->ids[0]};
        return af_join("durable_join", ids, 1, 10000);
    }
    af_result(doc);
    yyjson_doc_free(doc);
    assert(af_occurrences(r->request, "FINAL_ACTIVE_ACK") == 1);
    assert(!strstr(r->request, "PRELIM_ACTIVE_RESULT"));
    return af_text("ACTIVE_RECOVERY_DONE");
}

static void checkpoint(agent_fixture *f, const ah_event *event) {
    bool root = !strcmp(event->session_id, f->root);
    if (f->phase == 2 && root && !strcmp(event->type, "state") &&
        strstr(event->json, "waiting_agents"))
        _exit(77);
    if (f->phase == 4 && root && !strcmp(event->type, "tool.completed") &&
        strstr(event->json, "timed_out"))
        _exit(77);
    if (f->phase == 3 && root && !strcmp(event->type, "agent.message_queued") &&
        strstr(event->json, "@terminal:"))
        _exit(77);
    if (f->phase == 5 && !root && !strcmp(event->type, "model.completed"))
        _exit(77);
    if (f->phase == 6 && !root && !strcmp(event->type, "model.completed") &&
        strstr(event->json, "PRELIM_ACTIVE_RESULT"))
        _exit(77);
}

int agent_recovery_child(int argc, char **argv) {
    assert(argc == 5 && !strcmp(argv[1], "--crash"));
    agent_fixture f = {.reply = recovery_reply,
                       .database = argv[2],
                       .on_event = checkpoint,
                       .phase = (unsigned)strtoul(argv[4], NULL, 10)};
    if (f.phase == 6)
        f.reply = active_recovery_reply;
    memcpy(f.root, argv[3], AH_ID_CAP);
    if (f.phase == 1)
        f.crash_event = "agent.spawned";
    af_open(&f, 2, 8, 4);
    af_run(&f);
    af_close(&f);
    return 88; /* Every checkpoint must actually be reached. */
}

typedef struct {
    unsigned spawned, joined, delivered, completions, attempts;
} receipts;

static void count_events(void *context, const ah_event *event) {
    receipts *r = context;
    r->spawned += !strcmp(event->type, "agent.spawned");
    r->joined += !strcmp(event->type, "tool.completed") && strstr(event->json, "timed_out") != NULL;
    r->delivered +=
        !strcmp(event->type, "agent.message_received") && strstr(event->json, "@terminal:") != NULL;
    r->completions +=
        !strcmp(event->type, "model.completed") &&
        (strstr(event->json, "DURABLE_RESULT") || strstr(event->json, "FINAL_ACTIVE_ACK"));
    r->attempts += !strcmp(event->type, "model.started");
}

static void resume_root(agent_fixture *f) {
    yyjson_doc *doc = af_agents(f);
    yyjson_val *agents = yyjson_obj_get(yyjson_doc_get_root(doc), "agents"), *agent;
    size_t index, count;
    ah_error error = {0};
    yyjson_arr_foreach(agents, index, count, agent) {
        const char *id = yyjson_get_str(yyjson_obj_get(agent, "id"));
        const char *state = yyjson_get_str(yyjson_obj_get(agent, "state"));
        assert(id && state);
        if (!strcmp(id, f->root) && !strcmp(state, "stopped"))
            af_ok(ah_resume(f->engine, id, &error), &error);
    }
    yyjson_doc_free(doc);
}

static void kill_checkpoints(const char *executable) {
    for (unsigned phase = 1; phase <= 6; phase++) {
        char path[] = "/tmp/ah-agents-recovery-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        close(fd);
        agent_fixture f = {.reply = recovery_reply, .database = path, .phase = phase};
        if (phase == 6)
            f.reply = active_recovery_reply;
        af_open(&f, 2, 8, 4);
        af_create(&f, "RECOVERY_ROOT");
        ah_close(f.engine);
        f.engine = NULL;
        char checkpoint_id[16];
        snprintf(checkpoint_id, sizeof(checkpoint_id), "%u", phase);
        char *arguments[] = {(char *)executable, "--crash", path, f.root, checkpoint_id, NULL};
        pid_t process;
        assert(posix_spawn(&process, executable, NULL, NULL, arguments, environ) == 0);
        int status;
        assert(waitpid(process, &status, 0) == process);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
        af_open(&f, 2, 8, 4);
        resume_root(&f);
        af_run(&f);
        af_state(&f, f.root, "completed");
        yyjson_doc *doc = af_agents(&f);
        yyjson_val *agents = yyjson_obj_get(yyjson_doc_get_root(doc), "agents"), *agent;
        assert(yyjson_arr_size(agents) == 2);
        size_t index, count;
        receipts totals = {0};
        ah_error error = {0};
        yyjson_arr_foreach(agents, index, count, agent) {
            const char *id = yyjson_get_str(yyjson_obj_get(agent, "id"));
            assert(id);
            af_ok(ah_events(f.engine, id, 0, count_events, &totals, &error), &error);
        }
        assert(totals.spawned == 1 && totals.joined == 1 && totals.completions == 1);
        assert(totals.delivered == 1);
        yyjson_val *usage = yyjson_obj_get(yyjson_doc_get_root(doc), "usage");
        assert(yyjson_get_uint(yyjson_obj_get(usage, "attempts")) == totals.attempts);
        assert(yyjson_get_uint(yyjson_obj_get(usage, "unreported_attempts")) == totals.attempts);
        yyjson_doc_free(doc);
        af_reopen(&f);
        unsigned requests = f.count;
        af_run(&f);
        assert(f.count == requests);
        af_close(&f);
        af_remove_database(path);
    }
}

static char *mail_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        if (strstr(r->request, "WAKE_AFTER_REOPEN")) {
            assert(af_occurrences(r->request, "QUEUED_BEFORE_CLOSE") == 1);
            assert(af_occurrences(r->request, "WAKE_AFTER_REOPEN") == 1);
        }
        return af_text("MAIL_RESULT");
    }
    yyjson_doc *doc = af_receipt(r->request, "mail_spawn");
    if (!doc)
        return af_call("spawn", "mail_spawn", "{\"name\":\"mail\",\"message\":\"TASK_MAIL\"}");
    yyjson_doc_free(doc);
    af_spawn_id(r->request, "mail_spawn", f->ids[0]);
    return af_text("ROOT_DONE");
}

static void queued_mail_survives_reopen(void) {
    char path[] = "/tmp/ah-agents-mail-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    agent_fixture f = {.reply = mail_reply, .database = path};
    af_open(&f, 2, 8, 4);
    af_create(&f, "MAIL_ROOT");
    af_run(&f);
    ah_error error = {0};
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "durable-message", "QUEUED_BEFORE_CLOSE", false,
                        &error),
          &error);
    unsigned requests = f.count;
    af_reopen(&f);
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "durable-message", "QUEUED_BEFORE_CLOSE", false,
                        &error),
          &error);
    assert(ah_agent_send(f.engine, f.root, f.ids[0], "durable-message", "CONFLICT", false,
                         &error) == AH_INVALID);
    af_run(&f);
    assert(f.count == requests);
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "durable-wake", "WAKE_AFTER_REOPEN", true,
                        &error),
          &error);
    af_reopen(&f);
    af_run(&f);
    assert(f.count == requests + 2);
    af_reopen(&f);
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "durable-wake", "WAKE_AFTER_REOPEN", true,
                        &error),
          &error);
    af_run(&f);
    assert(f.count == requests + 2);
    af_close(&f);
    af_remove_database(path);
}

static char *stopped_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        assert(strstr(r->request, "EXPLICIT_NEW_WAKE"));
        return af_text("EXPLICITLY_RESTARTED");
    }
    af_spawn_id(r->request, "durable_spawn", f->ids[0]);
    yyjson_doc *doc = af_receipt(r->request, "stopped_join");
    if (!doc) {
        const char *ids[] = {f->ids[0]};
        return af_join("stopped_join", ids, 1, 0);
    }
    yyjson_val *agents = yyjson_obj_get(af_result(doc), "agents");
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_arr_get_first(agents), "state"), "stopped"));
    yyjson_doc_free(doc);
    return af_text("STOP_CONFIRMED");
}

static void stop_before_initial_delivery(const char *executable) {
    char path[] = "/tmp/ah-agents-stop-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    agent_fixture f = {.reply = stopped_reply, .database = path};
    af_open(&f, 2, 8, 4);
    af_create(&f, "STOP_BEFORE_DELIVERY");
    ah_close(f.engine);
    f.engine = NULL;
    char *arguments[] = {(char *)executable, "--crash", path, f.root, "1", NULL};
    pid_t process;
    assert(posix_spawn(&process, executable, NULL, NULL, arguments, environ) == 0);
    int status;
    assert(waitpid(process, &status, 0) == process);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
    af_open(&f, 2, 8, 4);
    yyjson_doc *doc = af_agents(&f);
    yyjson_val *agents = yyjson_obj_get(yyjson_doc_get_root(doc), "agents");
    assert(yyjson_arr_size(agents) == 2);
    const char *child = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(agents, 1), "id"));
    assert(child);
    memcpy(f.ids[0], child, AH_ID_CAP);
    yyjson_doc_free(doc);
    ah_error error = {0};
    af_ok(ah_agent_interrupt(f.engine, f.root, f.ids[0], &error), &error);
    resume_root(&f);
    af_run(&f);
    assert(!af_session_calls(&f, f.ids[0]));
    af_state(&f, f.ids[0], "stopped");
    af_ok(ah_agent_send(f.engine, f.root, f.ids[0], "restart", "EXPLICIT_NEW_WAKE", true, &error),
          &error);
    af_run(&f);
    assert(af_session_calls(&f, f.ids[0]) == 1);
    af_state(&f, f.ids[0], "completed");
    af_close(&f);
    af_remove_database(path);
}

void agent_recovery_cases(const char *executable) {
    queued_mail_survives_reopen();
    kill_checkpoints(executable);
    stop_before_initial_delivery(executable);
}
