#include "scheduler.h"

typedef struct {
    ah_engine *engine;
    char root[AH_ID_CAP];
    agent_job **jobs;
    size_t count, capacity;
    unsigned active;
    uint64_t deadline;
    ah_code poll_error;
    ah_error poll_failure;
} agent_scheduler;

static void pump_models(void *context) {
    agent_scheduler *scheduler = context;
    if (scheduler->poll_error != AH_OK)
        return;
    ah_engine *e = scheduler->engine;
    uint64_t tool_deadline = e->deadline;
    e->deadline = scheduler->deadline;
    scheduler->poll_error =
        e->agents.provider.poll(e->agents.provider.context, 0, &scheduler->poll_failure);
    e->deadline = tool_deadline;
}

void ah_agents_config_init(ah_agents_config *config) {
    if (config)
        *config = (ah_agents_config){.max_active = 4, .max_agents = 32, .max_depth = 8};
}

ah_code ah_agents_enable(ah_engine *e, const ah_agents_config *config, ah_error *error) {
    ah_code code = core_guard(e, error);
    if (code != AH_OK)
        return code;
    if (!config || !config->provider.start || !config->provider.poll || !config->provider.result ||
        !config->provider.release || !config->max_active || config->max_active > 32 ||
        !config->max_agents || config->max_agents > 1024 || !config->max_depth ||
        config->max_depth > 32)
        return ah_fail(error, AH_INVALID, "async provider and bounded agent limits required");
    for (size_t i = 0; i < e->tool_count; ++i)
        if (agents_tool_schema(e->tools[i].name))
            return ah_fail(error, AH_INVALID, "registered tool conflicts with agent tool name");
    e->agents = *config;
    e->agents_enabled = true;
    ah_error_clear(error);
    return AH_OK;
}

/* Identity rows are append-only. Their stable order lets refresh update scalar
 * state without copying prompts or scanning the job vector for every child. */
static ah_code refresh(agent_scheduler *scheduler, ah_error *error) {
    ah_engine *e = scheduler->engine;
    sqlite3_stmt *stmt = NULL;
    ah_code code = core_prepare(
        e,
        "SELECT n.session_id,n.generation,n.resume_pending,s.state,s.turn,s.model_calls,"
        "s.tool_calls FROM agent_nodes n JOIN sessions s ON s.id=n.session_id "
        "WHERE n.root_id=? ORDER BY n.rowid",
        &stmt, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(stmt, 1, scheduler->root, -1, SQLITE_TRANSIENT);
    size_t index = 0;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        if (index == scheduler->count) {
            if (index == scheduler->capacity) {
                size_t capacity = scheduler->capacity ? 2 * scheduler->capacity : 8;
                agent_job **jobs = realloc(scheduler->jobs, capacity * sizeof(*jobs));
                if (!jobs) {
                    code = ah_fail(error, AH_IO, "cannot allocate agent scheduler");
                    break;
                }
                scheduler->jobs = jobs;
                scheduler->capacity = capacity;
            }
            agent_job *job = calloc(1, sizeof(*job));
            if (!job) {
                code = ah_fail(error, AH_IO, "cannot allocate agent execution");
                break;
            }
            job->engine = e;
            job->deadline = scheduler->deadline;
            code = core_load(e, id, &job->session, error);
            if (code != AH_OK) {
                free(job);
                break;
            }
            scheduler->jobs[scheduler->count++] = job;
        }
        agent_job *job = scheduler->jobs[index++];
        if (strcmp(job->session.id, id)) {
            code = ah_fail(error, AH_PROTOCOL, "agent identity changed during execution");
            break;
        }
        job->generation = (uint64_t)sqlite3_column_int64(stmt, 1);
        job->recover = sqlite3_column_int(stmt, 2) != 0;
        job->session.state = (ah_state)sqlite3_column_int(stmt, 3);
        job->session.turn = (unsigned)sqlite3_column_int(stmt, 4);
        job->session.model_calls = (unsigned)sqlite3_column_int(stmt, 5);
        job->session.tool_calls = (unsigned)sqlite3_column_int(stmt, 6);
    }
    if (code == AH_OK && step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "cannot load agent tree");
    sqlite3_finalize(stmt);
    return code;
}

static ah_code recover_job(agent_job *job, ah_error *error) {
    ah_engine *e = job->engine;
    uint64_t after = 0;
    ah_code code = core_begin(e, &after, error);
    if (code == AH_OK && job->session.state == AH_STOPPED)
        code = core_state(e, job->session.id, AH_READY, error);
    sqlite3_stmt *stmt = NULL;
    if (code == AH_OK)
        code = core_prepare(e, "UPDATE agent_nodes SET resume_pending=0 WHERE session_id=?", &stmt,
                            error);
    if (code == AH_OK) {
        sqlite3_bind_text(stmt, 1, job->session.id, -1, SQLITE_TRANSIENT);
        code = core_done(e, stmt, error);
    }
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    if (code == AH_OK) {
        job->recover = false;
        if (job->session.state == AH_STOPPED)
            job->session.state = AH_READY;
    }
    return code;
}

static ah_code advance(agent_scheduler *scheduler, agent_job *job, ah_error *error) {
    ah_engine *e = scheduler->engine;
    core_session *session = &job->session;
    e->deadline = job->deadline;
    /* An earlier sibling may have interrupted this agent since the tree scan. */
    sqlite3_stmt *current = NULL;
    ah_code code = core_prepare(
        e,
        "SELECT s.state,n.generation,n.resume_pending FROM sessions s JOIN agent_nodes n "
        "ON n.session_id=s.id WHERE s.id=?",
        &current, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(current, 1, session->id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(current) == SQLITE_ROW) {
        session->state = (ah_state)sqlite3_column_int(current, 0);
        job->generation = (uint64_t)sqlite3_column_int64(current, 1);
        job->recover = sqlite3_column_int(current, 2) != 0;
    } else
        code = ah_fail(error, AH_IO, "agent disappeared during scheduling");
    sqlite3_finalize(current);
    if (code != AH_OK)
        return code;
    if (job->handle) {
        bool done = false;
        ah_code code = agents_model_finish(job, &done, error);
        if (done)
            scheduler->active--;
        return code;
    }
    code = job->recover ? recover_job(job, error) : AH_OK;
    if (code == AH_OK && !strcmp(session->id, scheduler->root) &&
        (session->state == AH_READY || session->state == AH_RUNNING ||
         session->state == AH_COMPLETED))
        code = core_drain_steering(e, session, session->state == AH_COMPLETED, error);
    bool changed = false;
    if (code == AH_OK)
        code = agents_drain(e, session, &changed, error);
    if (code != AH_OK)
        return code;
    if (session->state == AH_READY) {
        code = core_run_transition(e, session->id, AH_RUNNING, "run.started", "{}", error);
        if (code != AH_OK)
            return code;
        session->state = AH_RUNNING;
        job->cancelled = false;
    }
    if (session->state != AH_RUNNING && session->state != AH_WAITING_AGENTS)
        return agents_job_notify(job, error);
    core_operation operation;
    code = core_next_operation(e, session->id, &operation, error);
    if (code == AH_OK) {
        bool handled = false;
        code = core_execute_parallel(e, session, &operation, &handled, error);
        if (code == AH_OK && !handled)
            code = core_execute_operation(e, session, &operation, error);
        core_operation_free(&operation);
        return code;
    }
    if (code != AH_NOT_FOUND)
        return code;
    if (session->state == AH_WAITING_AGENTS)
        return ah_fail(error, AH_PROTOCOL, "agent wait has no pending operation");
    if (scheduler->active >= e->agents.max_active)
        return AH_OK;
    code = agents_model_start(job, error);
    if (code == AH_OK)
        scheduler->active++;
    return code;
}

static ah_code suspend_jobs(agent_scheduler *scheduler, ah_error *error) {
    ah_engine *e = scheduler->engine;
    ah_code code = AH_OK;
    for (size_t i = 0; i < scheduler->count; ++i) {
        agent_job *job = scheduler->jobs[i];
        job->cancelled = true;
        agents_model_release(job);
        if (job->session.state != AH_RUNNING && job->session.state != AH_READY &&
            job->session.state != AH_WAITING_AGENTS &&
            (i != 0 || job->session.state != AH_COMPLETED))
            continue;
        uint64_t after = 0;
        ah_code saved = core_begin(e, &after, error);
        sqlite3_stmt *stmt = NULL;
        if (saved == AH_OK)
            saved = core_prepare(e, "UPDATE agent_nodes SET resume_pending=1 WHERE session_id=?",
                                 &stmt, error);
        if (saved == AH_OK) {
            sqlite3_bind_text(stmt, 1, job->session.id, -1, SQLITE_TRANSIENT);
            saved = core_done(e, stmt, error);
        }
        if (saved == AH_OK)
            saved = core_state(e, job->session.id, AH_STOPPED, error);
        if (saved == AH_OK)
            saved = core_event(e, job->session.id, NULL, "agent.suspended", "{}", error);
        if (saved == AH_OK)
            saved = core_commit(e, after, error);
        else
            core_rollback(e);
        if (saved != AH_OK)
            code = saved;
    }
    return code;
}

ah_code agents_run(ah_engine *e, const char *sid, ah_error *error) {
    ah_code code = core_guard(e, error);
    if (code != AH_OK)
        return code;
    code = agents_register_root(e, sid, error);
    agent_scheduler scheduler = {.engine = e};
    if (code == AH_OK)
        code = agents_root(e, sid, scheduler.root, error);
    uint64_t now = ah_now_ms();
    scheduler.deadline =
        e->config.run_timeout_ms > UINT64_MAX - now ? UINT64_MAX : now + e->config.run_timeout_ms;
    if (code == AH_OK)
        code = core_task_deadline(e, scheduler.root, &scheduler.deadline, error);
    if (code == AH_OK)
        code = refresh(&scheduler, error);
    if (code != AH_OK)
        goto cleanup;
    if (!scheduler.count) {
        code = ah_fail(error, AH_NOT_FOUND, "agent root unavailable");
        goto cleanup;
    }
    ah_state initial = scheduler.jobs[0]->session.state;
    if (initial == AH_IDLE || initial == AH_STOPPED || initial == AH_FAILED ||
        initial == AH_OUTCOME_UNKNOWN) {
        code = ah_fail(error, initial == AH_OUTCOME_UNKNOWN ? AH_UNCERTAIN : AH_INVALID,
                       "submit or resume the root before running agents");
        goto cleanup;
    }
    e->running = true;
    e->yield = pump_models;
    e->yield_context = &scheduler;
    core_mailbox_active(e, scheduler.root);
    while (code == AH_OK) {
        e->deadline = scheduler.deadline;
        if (core_cancelled(e)) {
            code = ah_fail(error,
                           atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                     : AH_LIMIT,
                           "agent tree cancelled or timed out");
            break;
        }
        code = refresh(&scheduler, error);
        if (code != AH_OK)
            break;
        /* Restore interrupted children before a parent's join can mistake the
         * recovery marker for an explicit stop that needs its attention. */
        for (size_t i = 0; i < scheduler.count && code == AH_OK; ++i)
            if (scheduler.jobs[i]->recover)
                code = recover_job(scheduler.jobs[i], error);
        for (size_t i = 0; i < scheduler.count && code == AH_OK; ++i) {
            agent_job *job = scheduler.jobs[i];
            ah_state state = job->session.state;
            if (!job->handle && (state == AH_COMPLETED || state == AH_IDLE || state == AH_STOPPED ||
                                 state == AH_FAILED)) {
                bool changed = false;
                code = agents_drain(e, &job->session, &changed, error);
            }
        }
        if (code != AH_OK)
            break;
        if (scheduler.poll_error != AH_OK) {
            code = scheduler.poll_error;
            if (error)
                *error = scheduler.poll_failure;
            break;
        }
        sqlite3_int64 before = sqlite3_total_changes64(e->db);
        unsigned active_before = scheduler.active;
        bool waiting = false;
        for (size_t i = 0; i < scheduler.count && code == AH_OK; ++i) {
            e->deadline = scheduler.deadline;
            if (core_cancelled(e)) {
                code =
                    ah_fail(error,
                            atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                      : AH_LIMIT,
                            "agent tree interrupted");
                break;
            }
            agent_job *job = scheduler.jobs[i];
            ah_error failure = {0};
            ah_code advanced = advance(&scheduler, job, &failure);
            if (advanced != AH_OK) {
                bool settled = false;
                advanced = core_run_failure(e, &job->session, advanced, &failure, &settled);
                ah_error notification = {0};
                ah_code notified = agents_job_notify(job, &notification);
                if (i == 0 || !settled || notified != AH_OK) {
                    code = notified != AH_OK ? notified : advanced;
                    if (error)
                        *error = notified != AH_OK ? notification : failure;
                }
            }
            waiting |= job->session.state == AH_WAITING_AGENTS;
        }
        if (code != AH_OK)
            break;
        bool changed =
            before != sqlite3_total_changes64(e->db) || active_before != scheduler.active;
        if (!scheduler.active && !changed && !waiting)
            break;
        e->deadline = scheduler.deadline;
        unsigned delay = changed ? 0 : 20;
        code = e->agents.provider.poll(e->agents.provider.context, delay, error);
    }
    if (code != AH_OK) {
        ah_error cleanup_error = {0};
        ah_code saved = suspend_jobs(&scheduler, &cleanup_error);
        if (saved != AH_OK) {
            code = saved;
            if (error)
                *error = cleanup_error;
        }
    }
    core_mailbox_active(e, NULL);
    e->deadline = 0;
    e->running = false;
    e->yield = NULL;
    e->yield_context = NULL;
cleanup:
    for (size_t i = 0; i < scheduler.count; ++i) {
        agents_model_release(scheduler.jobs[i]);
        core_session_free(&scheduler.jobs[i]->session);
        free(scheduler.jobs[i]);
    }
    free(scheduler.jobs);
    return code;
}
