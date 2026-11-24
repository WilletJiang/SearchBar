#ifndef SEARCHBAR_SEARCH_CORE_H
#define SEARCHBAR_SEARCH_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SearchBarIndex SearchBarIndex;
typedef struct SearchBarSearch SearchBarSearch;
typedef int (*SearchBarPathPredicate)(const char *path, void *context);

typedef struct {
    char path[4096];
    char name[512];
    char snippet[512];
    int kind;
    double score;
    int content_match;
} SearchBarHit;

/* Presentation strings remain valid until the next read or search close. */
typedef struct {
    const char *path, *name, *snippet;
    int kind;
    double score;
    int content_match;
} SearchBarResult;

/* Reuse one read snapshot and candidate/ranking state across progressive stages. */
SearchBarSearch *searchbar_search_open(SearchBarIndex *index, const char *query, int kind_filter, int limit);
int searchbar_search_metadata(SearchBarSearch *search);
int searchbar_search_complete(SearchBarSearch *search);
int searchbar_search_finished(SearchBarSearch *search);
int searchbar_search_read(SearchBarSearch *search, int ordinal, SearchBarResult *result);
void searchbar_search_close(SearchBarSearch *search);
/* Run on the writer/reader queues respectively; old body indexes remain searchable until commit. */
int searchbar_index_upgrade(SearchBarIndex *index);
/* 1: ready or not needed, 0: unavailable (SQL remains usable), -1: interrupted.
   The optional metadata snapshot bounds its builder and file to 64 MiB each. */
int searchbar_index_prepare_snapshot(SearchBarIndex *index);

typedef struct {
    char path[4096];
    int64_t id, size;
    int mode, applications_only;
} SearchBarScanItem;
/* Filename jobs spill to SQLite; pending bodies are paged from their existing metadata. */
int searchbar_index_queue_scan(SearchBarIndex *index, const char *path, int mode, int applications_only);
int searchbar_index_next_scan(SearchBarIndex *index, SearchBarScanItem *item);
int searchbar_index_next_content(SearchBarIndex *index, int64_t after_id, SearchBarScanItem *item);
int searchbar_index_clear_scan_jobs(SearchBarIndex *index);

/* Use each handle on one serial queue. Separate reader/writer handles use WAL.
   Mutators return 1 on success and 0 on failure. Errors survive until next call.
   open(NULL) fails; error(NULL) describes the last open failure on this thread. */
SearchBarIndex *searchbar_index_open(const char *database_path);
void searchbar_index_close(SearchBarIndex *index);
const char *searchbar_index_error(SearchBarIndex *index);
int searchbar_index_begin(SearchBarIndex *index);
int searchbar_index_commit(SearchBarIndex *index);
int searchbar_index_rollback(SearchBarIndex *index);
int searchbar_index_upsert(SearchBarIndex *index, const char *path, const char *name,
                       const char *aliases, int kind, double mtime, int64_t size);
int searchbar_index_upsert_scanned(SearchBarIndex *index, const char *path, const char *name,
                              const char *aliases, int kind, double mtime, int64_t size,
                              int content_eligible);
int searchbar_index_set_content(SearchBarIndex *index, const char *path, const char *text);
/* Content is truncated at 32 KiB without splitting UTF-8. */
int searchbar_index_clear_content(SearchBarIndex *index);
/* Reclaims deleted text and index pages. Run outside a transaction while idle. */
int searchbar_index_compact(SearchBarIndex *index);
int searchbar_index_remove(SearchBarIndex *index, const char *path);
int searchbar_index_remove_tree(SearchBarIndex *index, const char *path);
/* Removes absent filesystem entries; access errors preserve the entry. */
int searchbar_index_prune_missing(SearchBarIndex *index);
/* Predicate runs synchronously. Its path is valid only for that callback. */
int searchbar_index_prune_excluded(SearchBarIndex *index, SearchBarPathPredicate predicate, void *context);
/* Query checks return -1 on error. Content is invalidated when metadata changes.
   set_content records the row's current mtime, including an empty extraction. */
int searchbar_index_needs_update(SearchBarIndex *index, const char *path,
                             double mtime, int64_t size);
int searchbar_index_content_needed(SearchBarIndex *index, const char *path, double mtime);
int64_t searchbar_index_count(SearchBarIndex *index);
int64_t searchbar_index_content_bytes(SearchBarIndex *index);
/* filter -1: all, 0: files/folders, 2: applications. At most 256 hits.
   Search returns hit count, or -1 on error. Empty/symbol-only queries return 0.
   Queries accept fewer than 2048 UTF-8 bytes and at most 24 terms.
   UTF-8 results are NUL-terminated without splitting a code point. */
int searchbar_index_search(SearchBarIndex *index, const char *query, int kind_filter,
                       int limit, SearchBarHit *out_hits);
/* Same query/filter/error rules; skips typo/subsequence expansion and document
   content. Metadata hits have content_match=0 and an empty snippet. */
int searchbar_index_search_metadata(SearchBarIndex *index, const char *query, int kind_filter,
                                int limit, SearchBarHit *out_hits);
/* Safe to call from another thread while a query is running. */
void searchbar_index_interrupt(SearchBarIndex *index);

#ifdef __cplusplus
}
#endif
#endif
