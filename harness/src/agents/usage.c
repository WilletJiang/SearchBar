#include "agents_internal.h"
#include <limits.h>

static ah_code tokens(yyjson_val *object, const char *key, bool required, unsigned *out,
                      ah_error *err) {
    yyjson_val *value = yyjson_obj_get(object, key);
    *out = 0;
    if (!value && !required)
        return AH_OK;
    /* Signed 32-bit per-response bounds keep exact SQLite sums safe across the
     * full unsigned model-call budget, including recovered attempts. */
    if (!yyjson_is_uint(value) || yyjson_get_uint(value) > INT32_MAX)
        return ah_fail(err, AH_PROTOCOL, "usage.%s must be a bounded nonnegative integer", key);
    *out = (unsigned)yyjson_get_uint(value);
    return AH_OK;
}

ah_code agents_usage_intent(ah_engine *e, const core_session *session, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "INSERT OR IGNORE INTO agent_usage(session_id,turn,attempt,"
                              "input_tokens,cached_input_tokens,output_tokens,reasoning_tokens,"
                              "reported) SELECT ?,?,?,0,0,0,0,0 WHERE EXISTS "
                              "(SELECT 1 FROM agent_nodes WHERE session_id=?)",
                              &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, session->turn);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)session->model_calls + 1);
        sqlite3_bind_text(s, 4, session->id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    return rc;
}

ah_code agents_record_usage(ah_engine *e, const core_session *session, yyjson_val *response,
                            ah_error *err) {
    char root[AH_ID_CAP];
    ah_code rc = agents_root(e, session->id, root, err);
    if (rc == AH_NOT_FOUND) {
        ah_error_clear(err);
        return AH_OK;
    }
    if (rc != AH_OK)
        return rc;
    yyjson_val *usage = yyjson_obj_get(response, "usage");
    bool reported = usage && !yyjson_is_null(usage);
    unsigned input = 0, cached = 0, output = 0, reasoning = 0;
    if (reported) {
        if (!yyjson_is_obj(usage))
            return ah_fail(err, AH_PROTOCOL, "usage must be an object or null");
        yyjson_val *in_details = yyjson_obj_get(usage, "input_tokens_details");
        yyjson_val *out_details = yyjson_obj_get(usage, "output_tokens_details");
        if ((in_details && !yyjson_is_obj(in_details)) ||
            (out_details && !yyjson_is_obj(out_details)))
            return ah_fail(err, AH_PROTOCOL, "token usage details must be objects");
        rc = tokens(usage, "input_tokens", true, &input, err);
        if (rc == AH_OK)
            rc = tokens(usage, "output_tokens", true, &output, err);
        if (rc == AH_OK)
            rc = tokens(in_details, "cached_tokens", false, &cached, err);
        if (rc == AH_OK)
            rc = tokens(out_details, "reasoning_tokens", false, &reasoning, err);
        if (rc != AH_OK)
            return rc;
        if (cached > input || reasoning > output)
            return ah_fail(err, AH_PROTOCOL, "detailed token usage exceeds its total");
    }
    sqlite3_stmt *s = NULL;
    rc = core_prepare(
        e,
        "INSERT INTO agent_usage(session_id,turn,attempt,input_tokens,"
        "cached_input_tokens,output_tokens,reasoning_tokens,reported) "
        "VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(session_id,turn,attempt) DO UPDATE SET "
        "input_tokens=excluded.input_tokens,cached_input_tokens=excluded.cached_input_tokens,"
        "output_tokens=excluded.output_tokens,reasoning_tokens=excluded.reasoning_tokens,"
        "reported=1 WHERE agent_usage.reported=0 AND excluded.reported=1",
        &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, session->turn);
        sqlite3_bind_int64(s, 3, session->model_calls);
        sqlite3_bind_int64(s, 4, input);
        sqlite3_bind_int64(s, 5, cached);
        sqlite3_bind_int64(s, 6, output);
        sqlite3_bind_int64(s, 7, reasoning);
        sqlite3_bind_int(s, 8, reported);
        rc = core_done(e, s, err);
    }
    return rc;
}

yyjson_mut_val *agents_usage_value(ah_engine *e, yyjson_mut_doc *d, const char *id, bool tree,
                                   ah_error *err) {
    sqlite3_stmt *s = NULL;
    const char *sql =
        tree ? "SELECT COUNT(*),COALESCE(SUM(u.reported),0),COALESCE(SUM(u.input_tokens),0),"
               "COALESCE(SUM(u.cached_input_tokens),0),COALESCE(SUM(u.output_tokens),0),"
               "COALESCE(SUM(u.reasoning_tokens),0) FROM agent_usage u JOIN agent_nodes n "
               "ON n.session_id=u.session_id WHERE n.root_id=?"
             : "SELECT COUNT(*),COALESCE(SUM(reported),0),COALESCE(SUM(input_tokens),0),"
               "COALESCE(SUM(cached_input_tokens),0),COALESCE(SUM(output_tokens),0),"
               "COALESCE(SUM(reasoning_tokens),0) FROM agent_usage WHERE session_id=?";
    ah_code rc = core_prepare(e, sql, &s, err);
    if (rc != AH_OK)
        return NULL;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    yyjson_mut_val *value = NULL;
    if (sqlite3_step(s) != SQLITE_ROW)
        ah_fail(err, AH_IO, "cannot aggregate agent usage");
    else {
        value = yyjson_mut_obj(d);
        const char *keys[] = {"attempts",      "reported_responses",
                              "input_tokens",  "cached_input_tokens",
                              "output_tokens", "reasoning_tokens"};
        for (int i = 0; i < 6; i++)
            yyjson_mut_obj_add_uint(d, value, keys[i], (uint64_t)sqlite3_column_int64(s, i));
        yyjson_mut_obj_add_uint(
            d, value, "unreported_attempts",
            (uint64_t)(sqlite3_column_int64(s, 0) - sqlite3_column_int64(s, 1)));
    }
    sqlite3_finalize(s);
    return value;
}
