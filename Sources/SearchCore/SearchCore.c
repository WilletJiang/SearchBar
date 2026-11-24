#include "SearchIndex.h"

#include "SearchTokenizer.h"
#include "SearchQuery.h"
#include <sqlite3.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_BODY_BYTES 32768

static _Thread_local char open_error[512];

void searchbar_copy_utf8(char *out, size_t capacity, const char *text) {
    if (!capacity) return;
    if (!text) text = "";
    size_t bytes = strlen(text);
    if (bytes >= capacity) {
        bytes = capacity - 1;
        while (bytes && ((unsigned char)text[bytes] & 0xc0) == 0x80) --bytes;
    }
    memcpy(out, text, bytes);
    out[bytes] = 0;
}

int searchbar_fail(SearchBarIndex *index, const char *message) {
    searchbar_copy_utf8(index ? index->error : open_error, 512, message);
    return 0;
}

int searchbar_sql_error(SearchBarIndex *index) {
    return searchbar_fail(index, sqlite3_errmsg(index->db));
}

int searchbar_execute(SearchBarIndex *index, const char *sql) {
    if (sqlite3_exec(index->db, sql, NULL, NULL, NULL) != SQLITE_OK) return searchbar_sql_error(index);
    return 1;
}

static int register_tokenizers(SearchBarIndex *index) {
    if (searchbar_register_tokenizers(index->db) == SQLITE_OK) return 1;
    return searchbar_fail(index, "Could not register SQLite FTS5 tokenizers");
}

int searchbar_prepare(SearchBarIndex *index, const char *sql, sqlite3_stmt **out) {
    if (sqlite3_prepare_v2(index->db, sql, -1, out, NULL) != SQLITE_OK) return searchbar_sql_error(index);
    return 1;
}

SearchBarIndex *searchbar_index_open(const char *path) {
    open_error[0] = 0;
    if (!path || !*path) { searchbar_fail(NULL, "Database path is empty"); return NULL; }
    SearchBarIndex *index = calloc(1, sizeof(*index));
    if (!index) { searchbar_fail(NULL, "Out of memory"); return NULL; }
    if (sqlite3_open_v2(path, &index->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                       SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        searchbar_sql_error(index); goto failed;
    }
    atomic_init(&index->interrupt_epoch, 0);
    sqlite3_busy_timeout(index->db, 3000);
    sqlite3_limit(index->db, SQLITE_LIMIT_LENGTH, 1024 * 1024);
    if (!register_tokenizers(index) ||
        !searchbar_execute(index, "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;"
                        "PRAGMA cache_size=-2048;PRAGMA mmap_size=8388608;"
                        "PRAGMA temp_store=MEMORY;PRAGMA wal_autocheckpoint=512;") ||
        !searchbar_schema_open(index)) goto failed;
    if (!searchbar_prepare(index,
        "INSERT INTO entries(path,name,aliases,kind,mtime,size,nname,npath,naliases,content_eligible)"
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,max(?10,0)) ON CONFLICT(path) DO UPDATE SET "
        "name=excluded.name,aliases=excluded.aliases,kind=excluded.kind,"
        "content_eligible=CASE WHEN ?10<0 THEN entries.content_eligible ELSE excluded.content_eligible END,"
        "nname=excluded.nname,npath=excluded.npath,naliases=excluded.naliases,"
        "body=CASE WHEN entries.mtime=excluded.mtime AND entries.size=excluded.size THEN entries.body END,"
        "body_offsets=CASE WHEN entries.mtime=excluded.mtime AND entries.size=excluded.size THEN entries.body_offsets END,"
        "content_mtime=CASE WHEN entries.mtime=excluded.mtime AND entries.size=excluded.size THEN entries.content_mtime END,"
        "mtime=excluded.mtime,size=excluded.size", &index->upsert) ||
        !searchbar_prepare(index, "UPDATE entries SET body=?2,body_offsets=searchbar_offsets(?2),content_mtime=mtime,content_eligible=1 WHERE path=?1", &index->set_content) ||
        !searchbar_prepare(index, "SELECT mtime,size,content_mtime FROM entries WHERE path=?1", &index->metadata)) goto failed;
    return index;
failed:
    searchbar_copy_utf8(open_error, sizeof(open_error), index->error);
    searchbar_index_close(index);
    return NULL;
}

void searchbar_index_close(SearchBarIndex *index) {
    if (!index) return;
    sqlite3_finalize(index->upsert);
    sqlite3_finalize(index->set_content);
    sqlite3_finalize(index->metadata);
    for (int i = 0; i < 9; ++i) sqlite3_finalize(index->search_statements[i]);
    for (int i = 0; i < 4; ++i) sqlite3_finalize(index->scan_statements[i]);
    searchbar_snapshot_close(index->snapshot);
    if (index->db) sqlite3_close_v2(index->db);
    free(index);
}

const char *searchbar_index_error(SearchBarIndex *index) { return index ? index->error : open_error; }
int searchbar_index_begin(SearchBarIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    return searchbar_execute(index, "BEGIN IMMEDIATE");
}
int searchbar_index_commit(SearchBarIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    return searchbar_execute(index, "COMMIT");
}
int searchbar_index_rollback(SearchBarIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    return searchbar_execute(index, "ROLLBACK");
}

static int finish_mutation(SearchBarIndex *index, sqlite3_stmt *statement) {
    int result = sqlite3_step(statement);
    int ok = result == SQLITE_DONE;
    if (!ok) searchbar_sql_error(index);
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return ok;
}

static int upsert(SearchBarIndex *index, const char *path, const char *name,
                  const char *aliases, int kind, double mtime, int64_t size, int eligible) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!path || !*path || !name || kind < 0 || kind > 2 || !isfinite(mtime) || size < 0)
        return searchbar_fail(index, "Invalid entry metadata");
    if (strlen(path) >= sizeof(((SearchBarHit *)0)->path) || strlen(name) > 65536 ||
        (aliases && strlen(aliases) > 65536))
        return searchbar_fail(index, "Entry metadata is too long");
    char *nname = searchbar_fold_text(name), *npath = searchbar_fold_text(path), *naliases = searchbar_fold_text(aliases);
    if (!nname || !npath || !naliases) {
        free(nname); free(npath); free(naliases);
        return searchbar_fail(index, "Invalid UTF-8 or out of memory");
    }
    sqlite3_stmt *statement = index->upsert;
    sqlite3_bind_text(statement, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, aliases ? aliases : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, kind);
    sqlite3_bind_double(statement, 5, mtime);
    sqlite3_bind_int64(statement, 6, size);
    sqlite3_bind_text(statement, 7, nname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 8, npath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 9, naliases, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 10, eligible);
    free(nname); free(npath); free(naliases);
    return finish_mutation(index, statement);
}

int searchbar_index_upsert(SearchBarIndex *index, const char *path, const char *name,
                       const char *aliases, int kind, double mtime, int64_t size) {
    return upsert(index, path, name, aliases, kind, mtime, size, -1);
}
int searchbar_index_upsert_scanned(SearchBarIndex *index, const char *path, const char *name,
                              const char *aliases, int kind, double mtime, int64_t size, int eligible) {
    return upsert(index, path, name, aliases, kind, mtime, size, eligible != 0);
}

int searchbar_index_set_content(SearchBarIndex *index, const char *path, const char *text) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!path || !*path) return searchbar_fail(index, "Content path is empty");
    if (!text) text = "";
    char bounded[MAX_BODY_BYTES + 1];
    if (strlen(text) > MAX_BODY_BYTES) {
        searchbar_copy_utf8(bounded, sizeof(bounded), text);
        text = bounded;
    }
    char *valid = searchbar_fold_text(text);
    if (!valid) return searchbar_fail(index, "Invalid UTF-8 or out of memory");
    free(valid);
    sqlite3_bind_text(index->set_content, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(index->set_content, 2, text, -1, SQLITE_TRANSIENT);
    if (!finish_mutation(index, index->set_content)) return 0;
    if (!sqlite3_changes(index->db)) return searchbar_fail(index, "Content entry does not exist");
    return 1;
}

int searchbar_index_clear_content(SearchBarIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    return searchbar_execute(index, "UPDATE entries SET body=NULL,body_offsets=NULL,content_mtime=NULL WHERE body IS NOT NULL");
}

int searchbar_index_compact(SearchBarIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!sqlite3_get_autocommit(index->db)) return searchbar_fail(index, "Compaction requires no open transaction");
    if (!searchbar_execute(index, "INSERT INTO meta(meta) VALUES('optimize');"
                        "INSERT INTO fulltext(fulltext) VALUES('optimize');VACUUM;")) return 0;
    int result = sqlite3_wal_checkpoint_v2(index->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    if (result != SQLITE_OK) return searchbar_sql_error(index);
    return 1;
}

int searchbar_index_remove(SearchBarIndex *index, const char *path) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!path) return searchbar_fail(index, "Removal path is missing");
    sqlite3_stmt *statement;
    if (!searchbar_prepare(index, "DELETE FROM entries WHERE path=?1", &statement)) return 0;
    sqlite3_bind_text(statement, 1, path, -1, SQLITE_TRANSIENT);
    int ok = finish_mutation(index, statement);
    sqlite3_finalize(statement);
    return ok;
}

int searchbar_index_remove_tree(SearchBarIndex *index, const char *path) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!path || !*path) return searchbar_fail(index, "Removal path is empty");
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') --length;
    char *prefix = malloc(length + 2), *upper = malloc(length + 2);
    if (!prefix || !upper) { free(prefix); free(upper); return searchbar_fail(index, "Out of memory"); }
    memcpy(prefix, path, length);
    prefix[length] = '/'; prefix[length + 1] = 0;
    if (length == 1 && path[0] == '/') prefix[1] = 0;
    strcpy(upper, prefix); upper[strlen(upper) - 1] = '0';
    sqlite3_stmt *statement;
    int ok = searchbar_prepare(index, "DELETE FROM entries WHERE path=?1 OR (path>=?2 AND path<?3)", &statement);
    if (ok) {
        sqlite3_bind_text(statement, 1, path, (int)length, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, prefix, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, upper, -1, SQLITE_TRANSIENT);
        ok = finish_mutation(index, statement);
        sqlite3_finalize(statement);
    }
    free(prefix); free(upper);
    return ok;
}

static int prune_matching(SearchBarIndex *index, SearchBarPathPredicate predicate, void *context) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!predicate) return searchbar_fail(index, "Pruning predicate is missing");
    sqlite3_stmt *rows, *remove;
    if (!searchbar_prepare(index, "SELECT id,path FROM entries", &rows)) return 0;
    if (!searchbar_prepare(index, "DELETE FROM entries WHERE id=?1", &remove)) {
        sqlite3_finalize(rows); return 0;
    }
    int owned_transaction = sqlite3_get_autocommit(index->db);
    if (owned_transaction && !searchbar_execute(index, "BEGIN IMMEDIATE")) {
        sqlite3_finalize(rows); sqlite3_finalize(remove); return 0;
    }
    int result, ok = 1;
    while ((result = sqlite3_step(rows)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(rows, 1);
        if (!predicate(path, context)) continue;
        sqlite3_bind_int64(remove, 1, sqlite3_column_int64(rows, 0));
        if (!finish_mutation(index, remove)) { ok = 0; break; }
    }
    if (ok && result != SQLITE_DONE) { searchbar_sql_error(index); ok = 0; }
    sqlite3_finalize(rows); sqlite3_finalize(remove);
    if (owned_transaction) {
        if (ok) ok = searchbar_execute(index, "COMMIT");
        else sqlite3_exec(index->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return ok;
}

static int path_is_missing(const char *path, void *context) {
    (void)context;
    struct stat status;
    return lstat(path, &status) != 0 && (errno == ENOENT || errno == ENOTDIR);
}

int searchbar_index_prune_missing(SearchBarIndex *index) {
    return prune_matching(index, path_is_missing, NULL);
}

int searchbar_index_prune_excluded(SearchBarIndex *index, SearchBarPathPredicate predicate, void *context) {
    return prune_matching(index, predicate, context);
}

static int metadata_check(SearchBarIndex *index, const char *path, double mtime, int64_t size, int content) {
    if (!index) return -1;
    index->error[0] = 0;
    if (!path || !isfinite(mtime)) { searchbar_fail(index, "Invalid metadata check"); return -1; }
    sqlite3_bind_text(index->metadata, 1, path, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(index->metadata);
    int needed = 1;
    if (result == SQLITE_ROW) {
        needed = sqlite3_column_double(index->metadata, 0) != mtime;
        if (content) needed |= sqlite3_column_type(index->metadata, 2) == SQLITE_NULL ||
                              sqlite3_column_double(index->metadata, 2) != mtime;
        else needed |= sqlite3_column_int64(index->metadata, 1) != size;
    } else if (result != SQLITE_DONE) { searchbar_sql_error(index); needed = -1; }
    sqlite3_reset(index->metadata);
    sqlite3_clear_bindings(index->metadata);
    return needed;
}

int searchbar_index_needs_update(SearchBarIndex *index, const char *path, double mtime, int64_t size) {
    return metadata_check(index, path, mtime, size, 0);
}
int searchbar_index_content_needed(SearchBarIndex *index, const char *path, double mtime) {
    return metadata_check(index, path, mtime, 0, 1);
}

static int64_t scalar(SearchBarIndex *index, const char *sql) {
    if (!index) return -1;
    index->error[0] = 0;
    sqlite3_stmt *statement;
    if (!searchbar_prepare(index, sql, &statement)) return -1;
    int result = sqlite3_step(statement);
    int64_t value = -1;
    if (result == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
    else searchbar_sql_error(index);
    sqlite3_finalize(statement);
    return value;
}
int64_t searchbar_index_count(SearchBarIndex *index) { return scalar(index, "SELECT row_count FROM stats WHERE id=1"); }
int64_t searchbar_index_content_bytes(SearchBarIndex *index) {
    return scalar(index, "SELECT content_bytes FROM stats WHERE id=1");
}
void searchbar_index_interrupt(SearchBarIndex *index) {
    if (!index) return;
    atomic_fetch_add_explicit(&index->interrupt_epoch, 1, memory_order_relaxed);
    sqlite3_interrupt(index->db);
}

int searchbar_index_prepare_snapshot(SearchBarIndex *index) {
    return index ? searchbar_snapshot_prepare(index) : 0;
}
