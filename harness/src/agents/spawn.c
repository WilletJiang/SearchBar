#include "agents_internal.h"

ah_code agents_spawn(ah_engine *e, const core_session *parent, const core_operation *op,
                     yyjson_val *args, char **result, ah_error *err) {
    const char *name = yyjson_get_str(yyjson_obj_get(args, "name"));
    const char *message = yyjson_get_str(yyjson_obj_get(args, "message"));
    if (!name || !*name || strlen(name) > 64 || !message || !*message ||
        strlen(message) > e->config.max_context_bytes)
        return ah_fail(err, AH_INVALID, "bounded agent name and initial message required");
    char root[AH_ID_CAP], child[AH_ID_CAP];
    ah_code rc = agents_root(e, parent->id, root, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_stmt *s = NULL;
    rc =
        core_prepare(e, "SELECT session_id,name FROM agent_nodes WHERE spawn_operation=?", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, op->id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    bool exists = step == SQLITE_ROW;
    if (exists)
        memcpy(child, sqlite3_column_text(s, 0), AH_ID_CAP);
    sqlite3_finalize(s);
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        return ah_fail(err, AH_IO, "cannot inspect spawn intent");
    if (!exists) {
        rc = core_prepare(e,
                          "SELECT depth,(SELECT COUNT(*)-1 FROM agent_nodes WHERE root_id=?) "
                          "FROM agent_nodes WHERE session_id=?",
                          &s, err);
        if (rc != AH_OK)
            return rc;
        sqlite3_bind_text(s, 1, root, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, parent->id, -1, SQLITE_TRANSIENT);
        step = sqlite3_step(s);
        unsigned depth = step == SQLITE_ROW ? (unsigned)sqlite3_column_int(s, 0) + 1 : 0;
        bool available = step == SQLITE_ROW && depth <= e->agents.max_depth &&
                         (uint64_t)sqlite3_column_int64(s, 1) < e->agents.max_agents;
        sqlite3_finalize(s);
        if (step != SQLITE_ROW)
            return ah_fail(err, AH_IO, "cannot inspect agent limits");
        if (!available)
            return ah_fail(err, AH_LIMIT, "agent count or depth limit exhausted");
        rc = core_id(child, err);
        if (rc == AH_OK)
            rc = core_prepare(
                e, "INSERT INTO sessions(id,model,instructions,state) VALUES(?,?,?,?)", &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, child, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, parent->model, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, parent->instructions, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 4, AH_IDLE);
            rc = core_done(e, s, err);
        }
        if (rc == AH_OK)
            rc = core_prepare(e,
                              "INSERT INTO agent_nodes(session_id,root_id,parent_id,name,"
                              "spawn_operation,depth) VALUES(?,?,?,?,?,?)",
                              &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, child, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, root, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, parent->id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, op->id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 6, depth);
            rc = core_done(e, s, err);
        }
        if (rc == AH_OK)
            rc = core_event(e, child, NULL, "session.created", "{}", err);
        if (rc == AH_OK)
            rc = agents_queue(e, parent->id, child, op->id, message, true, false, err);
        if (rc != AH_OK)
            return rc;
    }
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return ah_fail(err, AH_IO, "out of memory");
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_strcpy(d, o, "id", child);
    yyjson_mut_obj_add_strcpy(d, o, "name", name);
    *result = ah_mut_write(d);
    ah_mut_doc_free(d);
    if (!*result)
        return ah_fail(err, AH_IO, "out of memory");
    return exists ? AH_OK : core_event(e, parent->id, op->id, "agent.spawned", *result, err);
}
