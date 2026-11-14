#include "agents_internal.h"

ah_code agents_allow(ah_engine *e, const char *sid, bool model, unsigned count, unsigned *remaining,
                     ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e,
        model ? "SELECT t.model_used FROM agent_trees t JOIN agent_nodes n ON n.root_id=t.root_id "
                "WHERE n.session_id=?"
              : "SELECT t.tool_used FROM agent_trees t JOIN agent_nodes n ON n.root_id=t.root_id "
                "WHERE n.session_id=?",
        &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW) {
        uint64_t used = (uint64_t)sqlite3_column_int64(s, 0);
        unsigned limit = model ? e->config.max_model_calls : e->config.max_tool_calls;
        unsigned left = used >= limit ? 0 : limit - (unsigned)used;
        if (count > left)
            rc = ah_fail(err, AH_LIMIT, "agent tree %s budget exhausted", model ? "model" : "tool");
        if (remaining && *remaining > left)
            *remaining = left;
    } else if (step != SQLITE_DONE)
        rc = ah_fail(err, AH_IO, "cannot read agent budget");
    sqlite3_finalize(s);
    return rc;
}

ah_code agents_charge(ah_engine *e, const char *sid, bool model, unsigned count, ah_error *err) {
    ah_code rc = agents_allow(e, sid, model, count, NULL, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          model ? "UPDATE agent_trees SET model_used=model_used+? WHERE root_id="
                                  "(SELECT root_id FROM agent_nodes WHERE session_id=?)"
                                : "UPDATE agent_trees SET tool_used=tool_used+? WHERE root_id="
                                  "(SELECT root_id FROM agent_nodes WHERE session_id=?)",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_int64(s, 1, count);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    return rc;
}
