#ifndef SEARCHBAR_SEARCH_INDEX_H
#define SEARCHBAR_SEARCH_INDEX_H
#include "SearchCore.h"
#include "SearchQuery.h"
#include <sqlite3.h>
#include <stdatomic.h>
#include <stddef.h>
#define SEARCHBAR_MAX_HITS 256

typedef struct SearchBarSnapshot SearchBarSnapshot;
typedef struct {
    sqlite3_int64 id;
    double score;
    char *path;
    int content_match;
    int position;
} SearchBarRank;

struct SearchBarIndex {
    sqlite3 *db;
    sqlite3_stmt *upsert, *set_content, *metadata;
    sqlite3_stmt *search_statements[9];
    sqlite3_stmt *scan_statements[4];
    uint64_t checked_generation;
    unsigned char checked_identity[16];
    int snapshot_checked;
    SearchBarSnapshot *snapshot;
    atomic_uint_fast64_t interrupt_epoch;
    char error[512];
};

struct SearchBarSearch {
    SearchBarIndex *index;
    SearchQuery query;
    SearchBarRank ranks[SEARCHBAR_MAX_HITS];
    uint64_t epoch;
    int filter, limit, count, owns_transaction, positional, metadata_done, complete;
    char name[512], snippet[512];
};

void searchbar_copy_utf8(char *out, size_t capacity, const char *text);
const char *searchbar_column_text(sqlite3_stmt *statement, int column);
int searchbar_fail(SearchBarIndex *index, const char *message);
int searchbar_sql_error(SearchBarIndex *index);
int searchbar_schema_open(SearchBarIndex *index);
int searchbar_execute(SearchBarIndex *index, const char *sql);
int searchbar_prepare(SearchBarIndex *index, const char *sql, sqlite3_stmt **out);
int searchbar_search_cancelled(SearchBarSearch *search);
int searchbar_search_consider(SearchBarSearch *search, sqlite3_int64 id, const char *path,
                         double score, int content_match, int position);
void searchbar_snapshot_close(SearchBarSnapshot *snapshot);
int searchbar_snapshot_prepare(SearchBarIndex *index);
int searchbar_snapshot_search(SearchBarSearch *search);
#endif
