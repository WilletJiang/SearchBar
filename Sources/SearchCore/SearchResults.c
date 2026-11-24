#include "SearchIndex.h"
#include "SearchTokenizer.h"
#include <stdlib.h>
#include <string.h>

const char *searchbar_column_text(sqlite3_stmt *statement, int column) {
    const unsigned char *text = sqlite3_column_text(statement, column);
    return text ? (const char *)text : "";
}

static sqlite3_stmt *statement(SearchBarSearch *search, int slot, const char *sql) {
    SearchBarIndex *index = search->index;
    if (!index->search_statements[slot] && !searchbar_prepare(index, sql, &index->search_statements[slot])) return NULL;
    sqlite3_stmt *result = index->search_statements[slot];
    sqlite3_reset(result); sqlite3_clear_bindings(result);
    return result;
}

int searchbar_search_cancelled(SearchBarSearch *search) {
    if (atomic_load_explicit(&search->index->interrupt_epoch, memory_order_relaxed) == search->epoch) return 0;
    searchbar_fail(search->index, "Search interrupted");
    return 1;
}

int searchbar_search_consider(SearchBarSearch *search, sqlite3_int64 id, const char *path,
                         double score, int content_match, int position) {
    SearchBarRank *ranks = search->ranks;
    for (int i = 0; i < search->count; ++i) {
        if (ranks[i].id != id) continue;
        if (content_match) { ranks[i].content_match = 1; ranks[i].position = position; }
        return 1;
    }
    int at = 0;
    while (at < search->count && (ranks[at].score > score ||
           (ranks[at].score == score && strcmp(ranks[at].path, path) < 0))) ++at;
    if (at >= search->limit) return 1;
    char *copy = strdup(path);
    if (!copy) return searchbar_fail(search->index, "Out of memory");
    int count = search->count < search->limit ? search->count + 1 : search->count;
    if (search->count == search->limit) free(ranks[count - 1].path);
    memmove(ranks + at + 1, ranks + at, (size_t)(count - at - 1) * sizeof(*ranks));
    ranks[at] = (SearchBarRank){id, score, copy, content_match, position};
    search->count = count;
    return 1;
}

SearchBarSearch *searchbar_search_open(SearchBarIndex *index, const char *text, int filter, int limit) {
    if (!index) return NULL;
    index->error[0] = 0;
    if (filter != -1 && filter != 0 && filter != 2) { searchbar_fail(index, "Invalid search filter"); return NULL; }
    if (text && strlen(text) >= SEARCHBAR_QUERY_BYTES) { searchbar_fail(index, "Search query is too long"); return NULL; }
    SearchBarSearch *search = calloc(1, sizeof(*search));
    if (!search) { searchbar_fail(index, "Out of memory"); return NULL; }
    search->index = index; search->filter = filter;
    search->limit = limit < 0 ? 0 : limit > SEARCHBAR_MAX_HITS ? SEARCHBAR_MAX_HITS : limit;
    search->epoch = atomic_load_explicit(&index->interrupt_epoch, memory_order_relaxed);
    if (!searchbar_query_parse(&search->query, text)) {
        searchbar_fail(index, "Query cannot be normalized or has more than 24 terms");
        free(search); return NULL;
    }
    if (!search->query.count || !search->limit) { search->complete = 1; return search; }
    search->owns_transaction = sqlite3_get_autocommit(index->db);
    if (search->owns_transaction && !searchbar_execute(index, "BEGIN")) { free(search); return NULL; }
    sqlite3_stmt *mode = statement(search, 7,
        "SELECT instr(sql,'searchbar_body_v2') FROM sqlite_master WHERE name='fulltext'");
    if (!mode || sqlite3_step(mode) != SQLITE_ROW) {
        searchbar_sql_error(index); searchbar_search_close(search); return NULL;
    }
    search->positional = sqlite3_column_int(mode, 0) != 0;
    sqlite3_reset(mode);
    return search;
}

static int score_rows(SearchBarSearch *search, sqlite3_stmt *rows, int fuzzy) {
    int result, visited = 0;
    while ((result = sqlite3_step(rows)) == SQLITE_ROW) {
        if (!(visited++ & 31) && searchbar_search_cancelled(search)) { sqlite3_reset(rows); return 0; }
        double score = searchbar_query_score(&search->query, searchbar_column_text(rows, 2),
                                        searchbar_column_text(rows, 3), searchbar_column_text(rows, 4), fuzzy);
        if (score > 0 && !searchbar_search_consider(search, sqlite3_column_int64(rows, 0),
                                               searchbar_column_text(rows, 1), score, 0, 0)) {
            sqlite3_reset(rows); return 0;
        }
    }
    if (result != SQLITE_DONE) searchbar_sql_error(search->index);
    sqlite3_reset(rows);
    return result == SQLITE_DONE;
}

static int prefix_search(SearchBarSearch *search) {
    sqlite3_stmt *rows = statement(search, 0,
        "SELECT id,path,nname,npath,naliases FROM entries WHERE nname>=?1 AND nname<?2 "
        "AND (?3=-1 OR (?3=0 AND kind<>2) OR kind=?3) ORDER BY nname LIMIT 512");
    if (!rows) return 0;
    size_t bytes = strlen(search->query.prefix);
    char upper[SEARCHBAR_QUERY_BYTES + 1];
    memcpy(upper, search->query.prefix, bytes);
    upper[bytes] = (char)0xff; upper[bytes + 1] = 0;
    sqlite3_bind_text(rows, 1, search->query.prefix, -1, SQLITE_STATIC);
    sqlite3_bind_text(rows, 2, upper, (int)bytes + 1, SQLITE_TRANSIENT);
    sqlite3_bind_int(rows, 3, search->filter);
    return score_rows(search, rows, 0);
}

static int candidates(SearchBarSearch *search, int fuzzy) {
    sqlite3_stmt *rows = statement(search, 1,
        "SELECT e.id,e.path,e.nname,e.npath,e.naliases FROM meta JOIN entries e ON e.id=meta.rowid "
        "WHERE meta MATCH ?1 AND (?2=-1 OR (?2=0 AND e.kind<>2) OR e.kind=?2) LIMIT 4096");
    if (!rows) return 0;
    char *expression = searchbar_query_match(&search->query, 0, fuzzy);
    if (!expression) return searchbar_fail(search->index, "Out of memory");
    sqlite3_bind_text(rows, 1, expression, -1, SQLITE_TRANSIENT);
    sqlite3_free(expression);
    sqlite3_bind_int(rows, 2, search->filter);
    return score_rows(search, rows, fuzzy);
}

static int single_character(SearchBarSearch *search) {
    sqlite3_stmt *rows = statement(search, 2,
        "SELECT id,path,nname,npath,naliases FROM entries WHERE "
        "(?2=-1 OR (?2=0 AND kind<>2) OR kind=?2) AND "
        "(instr(nname,?1)>0 OR instr(naliases,?1)>0 OR instr(npath,?1)>0) LIMIT 4096");
    if (!rows) return 0;
    sqlite3_bind_text(rows, 1, search->query.terms[0], -1, SQLITE_STATIC);
    sqlite3_bind_int(rows, 2, search->filter);
    return score_rows(search, rows, 0);
}

int searchbar_search_metadata(SearchBarSearch *search) {
    if (!search) return -1;
    if (search->metadata_done || search->complete) return search->count;
    if (searchbar_search_cancelled(search)) return -1;
    int native = searchbar_snapshot_search(search);
    if (native < 0) return -1;
    if (!native) {
        if (!prefix_search(search)) return -1;
        int width;
        const char *term = search->query.terms[0];
        uint32_t first = searchbar_codepoint(term, (int)strlen(term), &width);
        int single = search->query.count == 1 && (int)strlen(term) == width && !searchbar_cjk_character(first);
        if (!(single ? single_character(search) : candidates(search, 0))) return -1;
    }
    search->metadata_done = 1;
    search->complete = search->count == search->limit && search->ranks[search->count - 1].score > 300;
    return search->count;
}

int searchbar_search_complete(SearchBarSearch *search) {
    if (!search || searchbar_search_metadata(search) < 0) return -1;
    if (search->complete) return search->count;
    if (searchbar_search_cancelled(search)) return -1;
    if (search->count < search->limit && !candidates(search, 1)) return -1;
    if (search->count == search->limit && search->ranks[search->count - 1].score > 300) {
        search->complete = 1; return search->count;
    }
    sqlite3_stmt *rows = statement(search, 3,
        "SELECT e.id,e.path,searchbar_position(fulltext),CASE WHEN ?3 THEN NULL ELSE e.body END "
        "FROM fulltext JOIN entries e ON e.id=fulltext.rowid WHERE fulltext MATCH ?1 "
        "AND (?2=-1 OR (?2=0 AND e.kind<>2) OR e.kind=?2) LIMIT 512");
    if (!rows) return -1;
    char *expression = searchbar_query_body_expression(&search->query, search->positional);
    if (!expression) { searchbar_fail(search->index, "Out of memory"); return -1; }
    sqlite3_bind_text(rows, 1, expression, -1, SQLITE_TRANSIENT); sqlite3_free(expression);
    sqlite3_bind_int(rows, 2, search->filter); sqlite3_bind_int(rows, 3, search->positional);
    int result;
    while ((result = sqlite3_step(rows)) == SQLITE_ROW) {
        if (searchbar_search_cancelled(search)) { sqlite3_reset(rows); return -1; }
        int matches = search->positional ? 1 : searchbar_query_body_matches(&search->query, searchbar_column_text(rows, 3));
        if (matches < 0) {
            searchbar_fail(search->index, "Cannot normalize document text"); sqlite3_reset(rows); return -1;
        }
        if (!matches) continue;
        if (!searchbar_search_consider(search, sqlite3_column_int64(rows, 0), searchbar_column_text(rows, 1),
                                   300, 1, sqlite3_column_int(rows, 2))) { sqlite3_reset(rows); return -1; }
        if (search->count == search->limit && search->ranks[search->count - 1].score >= 300) { result = SQLITE_DONE; break; }
    }
    if (result != SQLITE_DONE) searchbar_sql_error(search->index);
    sqlite3_reset(rows);
    search->complete = result == SQLITE_DONE;
    return result == SQLITE_DONE ? search->count : -1;
}

int searchbar_search_finished(SearchBarSearch *search) { return search && search->complete; }

int searchbar_search_read(SearchBarSearch *search, int ordinal, SearchBarResult *result) {
    if (!search || !result || ordinal < 0 || ordinal >= search->count || searchbar_search_cancelled(search)) return 0;
    SearchBarRank *rank = search->ranks + ordinal;
    sqlite3_stmt *row;
    if (rank->content_match && !search->positional) {
        row = statement(search, 6,
            "SELECT e.name,e.kind,searchbar_excerpt(fulltext) FROM fulltext JOIN entries e ON e.id=fulltext.rowid "
            "WHERE fulltext MATCH ?1 AND e.id=?2");
        if (!row) return 0;
        char *expression = searchbar_query_body_expression(&search->query, 0);
        if (!expression) return searchbar_fail(search->index, "Out of memory");
        sqlite3_bind_text(row, 1, expression, -1, SQLITE_TRANSIENT); sqlite3_free(expression);
        sqlite3_bind_int64(row, 2, rank->id);
    } else {
        row = statement(search, rank->content_match ? 5 : 4, rank->content_match ?
            "SELECT name,kind,body,body_offsets FROM entries WHERE id=?1" :
            "SELECT name,kind FROM entries WHERE id=?1");
        if (!row) return 0;
        sqlite3_bind_int64(row, 1, rank->id);
    }
    if (sqlite3_step(row) != SQLITE_ROW) { searchbar_sql_error(search->index); sqlite3_reset(row); return 0; }
    searchbar_copy_utf8(search->name, sizeof(search->name), searchbar_column_text(row, 0));
    search->snippet[0] = 0;
    if (rank->content_match) {
        if (search->positional) {
            int offset = searchbar_excerpt_offset(sqlite3_column_blob(row, 3), sqlite3_column_bytes(row, 3), rank->position);
            if (offset < 0) { searchbar_fail(search->index, "Missing document position map"); sqlite3_reset(row); return 0; }
            searchbar_excerpt_text(search->snippet, searchbar_column_text(row, 2), sqlite3_column_bytes(row, 2), offset);
        } else searchbar_copy_utf8(search->snippet, sizeof(search->snippet), searchbar_column_text(row, 2));
    }
    *result = (SearchBarResult){rank->path, search->name, search->snippet, sqlite3_column_int(row, 1),
                           rank->score, rank->content_match};
    sqlite3_reset(row);
    return 1;
}

void searchbar_search_close(SearchBarSearch *search) {
    if (!search) return;
    for (int i = 0; i < search->count; ++i) free(search->ranks[i].path);
    if (search->owns_transaction) sqlite3_exec(search->index->db, "COMMIT", NULL, NULL, NULL);
    free(search);
}

static int search_compatible(SearchBarIndex *index, const char *text, int filter, int limit, SearchBarHit *hits, int full) {
    if (!index) return -1;
    index->error[0] = 0;
    if (!text || !hits || limit <= 0) return 0;
    SearchBarSearch *search = searchbar_search_open(index, text, filter, limit);
    if (!search) return -1;
    int count = full ? searchbar_search_complete(search) : searchbar_search_metadata(search);
    for (int i = 0; i < count; ++i) {
        SearchBarResult result;
        if (!searchbar_search_read(search, i, &result)) { count = -1; break; }
        memset(hits + i, 0, sizeof(*hits));
        searchbar_copy_utf8(hits[i].path, sizeof(hits[i].path), result.path);
        searchbar_copy_utf8(hits[i].name, sizeof(hits[i].name), result.name);
        searchbar_copy_utf8(hits[i].snippet, sizeof(hits[i].snippet), result.snippet);
        hits[i].kind = result.kind; hits[i].score = result.score; hits[i].content_match = result.content_match;
    }
    searchbar_search_close(search);
    return count;
}

int searchbar_index_search(SearchBarIndex *index, const char *text, int filter, int limit, SearchBarHit *hits) {
    return search_compatible(index, text, filter, limit, hits, 1);
}
int searchbar_index_search_metadata(SearchBarIndex *index, const char *text, int filter, int limit, SearchBarHit *hits) {
    return search_compatible(index, text, filter, limit, hits, 0);
}
