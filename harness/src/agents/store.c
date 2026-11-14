#include "agents_internal.h"

ah_code agents_schema(ah_engine *e, ah_error *err) {
    return core_sql(
        e,
        "CREATE TABLE IF NOT EXISTS agent_nodes(session_id TEXT PRIMARY KEY REFERENCES "
        "sessions(id),root_id TEXT NOT NULL REFERENCES sessions(id),parent_id TEXT REFERENCES "
        "sessions(id),name TEXT NOT NULL,spawn_operation TEXT UNIQUE REFERENCES operations(id),"
        "depth INTEGER NOT NULL,generation INTEGER NOT NULL DEFAULT 0,"
        "resume_pending INTEGER NOT NULL DEFAULT 0,wake_after INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS agent_nodes_root ON agent_nodes(root_id);"
        "CREATE TABLE IF NOT EXISTS agent_trees(root_id TEXT PRIMARY KEY REFERENCES sessions(id),"
        "model_used INTEGER NOT NULL DEFAULT 0,tool_used INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS agent_messages(seq INTEGER PRIMARY KEY AUTOINCREMENT,"
        "sender TEXT NOT NULL REFERENCES sessions(id),recipient TEXT NOT NULL REFERENCES "
        "sessions(id),message_id TEXT NOT NULL,text TEXT NOT NULL,wake INTEGER NOT NULL,"
        "consumed INTEGER NOT NULL DEFAULT 0,continuation INTEGER NOT NULL DEFAULT 0,"
        "UNIQUE(sender,message_id));"
        "CREATE INDEX IF NOT EXISTS agent_messages_pending ON agent_messages(recipient,seq) "
        "WHERE consumed=0;"
        "CREATE TABLE IF NOT EXISTS agent_waits(operation_id TEXT PRIMARY KEY REFERENCES "
        "operations(id),deadline_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS agent_usage(session_id TEXT NOT NULL REFERENCES sessions(id),"
        "turn INTEGER NOT NULL,attempt INTEGER NOT NULL,input_tokens INTEGER NOT NULL,"
        "cached_input_tokens INTEGER NOT NULL,output_tokens INTEGER NOT NULL,"
        "reasoning_tokens INTEGER NOT NULL,reported INTEGER NOT NULL,"
        "PRIMARY KEY(session_id,turn,attempt));",
        err);
}

ah_code agents_root(ah_engine *e, const char *sid, char root[AH_ID_CAP], ah_error *err) {
    if (!sid)
        return ah_fail(err, AH_INVALID, "agent session required");
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e, "SELECT root_id FROM agent_nodes WHERE session_id=?", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(s, 0);
        if (!id || strlen(id) != AH_ID_CAP - 1)
            rc = ah_fail(err, AH_PROTOCOL, "corrupt agent root");
        else
            memcpy(root, id, AH_ID_CAP);
    } else
        rc = ah_fail(err, step == SQLITE_DONE ? AH_NOT_FOUND : AH_IO, "agent unavailable");
    sqlite3_finalize(s);
    return rc;
}

ah_code agents_tree_settled(ah_engine *e, const char *sid, bool *settled, ah_error *err) {
    *settled = false;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e,
        "SELECT NOT EXISTS(SELECT 1 FROM agent_nodes n JOIN sessions s ON s.id=n.session_id "
        "WHERE n.root_id=(SELECT root_id FROM agent_nodes WHERE session_id=?) "
        "AND s.state NOT IN (?,?,?,?))",
        &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, AH_IDLE);
    sqlite3_bind_int(s, 3, AH_COMPLETED);
    sqlite3_bind_int(s, 4, AH_STOPPED);
    sqlite3_bind_int(s, 5, AH_FAILED);
    if (sqlite3_step(s) == SQLITE_ROW)
        *settled = sqlite3_column_int(s, 0) != 0;
    else
        rc = ah_fail(err, AH_IO, "cannot inspect unsettled agent tree");
    sqlite3_finalize(s);
    return rc;
}

ah_code agents_register_root(ah_engine *e, const char *sid, ah_error *err) {
    char root[AH_ID_CAP];
    ah_code rc = agents_root(e, sid, root, err);
    if (rc == AH_OK)
        return AH_OK;
    if (rc != AH_NOT_FOUND)
        return rc;
    core_session session;
    rc = core_load(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "INSERT INTO agent_nodes(session_id,root_id,name,depth) "
                          "VALUES(?,?,'root',0)",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_prepare(e, "INSERT INTO agent_trees(root_id,model_used,tool_used) VALUES(?,?,?)",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, session.model_calls);
        sqlite3_bind_int64(s, 3, session.tool_calls);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_event(e, sid, NULL, "agent.root", "{}", err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    core_session_free(&session);
    if (rc == AH_OK)
        ah_error_clear(err);
    return rc;
}

ah_code agents_list(ah_engine *e, const char *sid, char **json, ah_error *err) {
    *json = NULL;
    char root[AH_ID_CAP];
    ah_code rc = agents_root(e, sid, root, err);
    if (rc != AH_OK)
        return rc;
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return ah_fail(err, AH_IO, "out of memory");
    yyjson_mut_val *o = yyjson_mut_obj(d), *array = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_strcpy(d, o, "root", root);
    yyjson_mut_obj_add_strcpy(d, o, "self", sid);
    yyjson_mut_obj_add_val(d, o, "agents", array);
    sqlite3_stmt *s = NULL;
    rc = core_prepare(e,
                      "SELECT n.session_id,n.parent_id,n.name,n.depth,n.generation,s.state,"
                      "s.turn FROM agent_nodes n JOIN sessions s ON s.id=n.session_id "
                      "WHERE n.root_id=? ORDER BY n.rowid LIMIT 1025",
                      &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, root, -1, SQLITE_TRANSIENT);
        int step;
        while ((step = sqlite3_step(s)) == SQLITE_ROW) {
            yyjson_mut_val *item = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, item, "id", (const char *)sqlite3_column_text(s, 0));
            const char *parent = (const char *)sqlite3_column_text(s, 1);
            if (parent)
                yyjson_mut_obj_add_strcpy(d, item, "parent", parent);
            else
                yyjson_mut_obj_add_null(d, item, "parent");
            yyjson_mut_obj_add_strcpy(d, item, "name", (const char *)sqlite3_column_text(s, 2));
            yyjson_mut_obj_add_uint(d, item, "depth", (uint64_t)sqlite3_column_int64(s, 3));
            yyjson_mut_obj_add_uint(d, item, "generation", (uint64_t)sqlite3_column_int64(s, 4));
            yyjson_mut_obj_add_str(d, item, "state", ah_state_name(sqlite3_column_int(s, 5)));
            yyjson_mut_obj_add_uint(d, item, "turn", (uint64_t)sqlite3_column_int64(s, 6));
            yyjson_mut_val *usage =
                agents_usage_value(e, d, (const char *)sqlite3_column_text(s, 0), false, err);
            if (!usage) {
                rc = err && err->code ? err->code : AH_IO;
                break;
            }
            yyjson_mut_obj_add_val(d, item, "usage", usage);
            yyjson_mut_arr_append(array, item);
        }
        if (rc == AH_OK && step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "cannot list agents");
    }
    sqlite3_finalize(s);
    s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e, "SELECT model_used,tool_used FROM agent_trees WHERE root_id=?", &s,
                          err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, root, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) != SQLITE_ROW)
            rc = ah_fail(err, AH_IO, "agent budget unavailable");
        else {
            yyjson_mut_obj_add_uint(d, o, "model_used", (uint64_t)sqlite3_column_int64(s, 0));
            yyjson_mut_obj_add_uint(d, o, "tool_used", (uint64_t)sqlite3_column_int64(s, 1));
        }
    }
    sqlite3_finalize(s);
    if (rc == AH_OK) {
        yyjson_mut_val *usage = agents_usage_value(e, d, root, true, err);
        if (usage)
            yyjson_mut_obj_add_val(d, o, "usage", usage);
        else
            rc = err && err->code ? err->code : AH_IO;
    }
    if (rc == AH_OK && !(*json = ah_mut_write(d)))
        rc = ah_fail(err, AH_IO, "out of memory");
    ah_mut_doc_free(d);
    return rc;
}

ah_code ah_agents_json(ah_engine *e, const char *sid, char **json, ah_error *err) {
    if (json)
        *json = NULL;
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!json || !e->agents_enabled)
        return ah_fail(err, AH_INVALID, "enabled agents and output required");
    return agents_list(e, sid, json, err);
}
