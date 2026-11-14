#include "core.h"
#include "../agents/agents_internal.h"
#include <time.h>

uint64_t core_wall_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t))
        return 0;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

ah_code core_task_allow(ah_engine *e, const char *sid, bool model, unsigned requested,
                        unsigned *remaining, ah_error *error) {
    if (e->agents_enabled) {
        ah_code allowed = agents_allow(e, sid, model, requested, remaining, error);
        if (allowed != AH_OK)
            return allowed;
    }
    sqlite3_stmt *s = NULL;
    ah_code code = core_prepare(
        e,
        model ? "SELECT model_limit-model_used,state,deadline_ms FROM tasks WHERE session_id IN "
                "(?1, (SELECT root_id FROM agent_nodes WHERE session_id=?1))"
              : "SELECT tool_limit-tool_used,state,deadline_ms FROM tasks WHERE session_id IN (?1, "
                "(SELECT root_id FROM agent_nodes WHERE session_id=?1))",
        &s, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    while (step == SQLITE_ROW) {
        sqlite3_int64 left = sqlite3_column_int64(s, 0);
        const char *state = (const char *)sqlite3_column_text(s, 1);
        if (!strcmp(state, "unknown") || !strcmp(state, "verifying"))
            code = ah_fail(error, AH_UNCERTAIN, "Task verifier effects require reconciliation");
        else if (strcmp(state, "active") || left < (sqlite3_int64)requested ||
                 (uint64_t)sqlite3_column_int64(s, 2) <= core_wall_ms())
            code = ah_fail(error, AH_LIMIT, "Task-wide budget exhausted or task already accepted");
        if (remaining && code == AH_OK && *remaining > (uint64_t)left)
            *remaining = (unsigned)left;
        if (code != AH_OK)
            break;
        step = sqlite3_step(s);
    }
    if (code == AH_OK && step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "Cannot load task budget");
    sqlite3_finalize(s);
    return code;
}

ah_code core_task_charge(ah_engine *e, const char *sid, bool model, unsigned count,
                         ah_error *error) {
    if (e->agents_enabled) {
        ah_code charged = agents_charge(e, sid, model, count, error);
        if (charged != AH_OK)
            return charged;
    }
    sqlite3_stmt *s = NULL;
    ah_code code =
        core_prepare(e,
                     model ? "UPDATE tasks SET model_used=model_used+? WHERE session_id IN (?2, "
                             "(SELECT root_id FROM agent_nodes WHERE session_id=?2))"
                           : "UPDATE tasks SET tool_used=tool_used+? WHERE session_id IN (?2, "
                             "(SELECT root_id FROM agent_nodes WHERE session_id=?2))",
                     &s, error);
    if (code == AH_OK) {
        sqlite3_bind_int64(s, 1, count);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        code = core_done(e, s, error);
    }
    return code;
}

ah_code core_task_deadline(ah_engine *e, const char *sid, uint64_t *deadline, ah_error *error) {
    sqlite3_stmt *s = NULL;
    ah_code code = core_prepare(e,
                                "SELECT deadline_ms FROM tasks WHERE session_id IN (?1, (SELECT "
                                "root_id FROM agent_nodes WHERE session_id=?1))",
                                &s, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    while (step == SQLITE_ROW) {
        uint64_t wall = core_wall_ms(), end = (uint64_t)sqlite3_column_int64(s, 0),
                 now = ah_now_ms();
        if (wall >= end || *deadline <= now)
            code = ah_fail(error, AH_LIMIT, "Task wall-time budget exhausted");
        else if (end - wall < *deadline - now)
            *deadline = now + (end - wall);
        if (code != AH_OK)
            break;
        step = sqlite3_step(s);
    }
    if (code == AH_OK && step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "Cannot load task deadline");
    sqlite3_finalize(s);
    return code;
}
