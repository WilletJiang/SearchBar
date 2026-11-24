#include "SearchCore.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) {
    struct timespec time; clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec * 1e-9;
}
static int compare(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return a < b ? -1 : a > b;
}

static void json_string(const char *text) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

// Load each release engine separately; use the same staged API sequence as its Swift caller.
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Usage: search-performance LIBRARY DATABASE QUERY...\n"); return 1; }
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    SearchBarIndex *(*open_index)(const char *) = dlsym(library, "searchbar_index_open");
    void (*close_index)(SearchBarIndex *) = dlsym(library, "searchbar_index_close");
    const char *(*error)(SearchBarIndex *) = dlsym(library, "searchbar_index_error");
    int (*upgrade)(SearchBarIndex *) = dlsym(library, "searchbar_index_upgrade");
    int (*prepare)(SearchBarIndex *) = dlsym(library, "searchbar_index_prepare_snapshot");
    int (*metadata)(SearchBarIndex *, const char *, int, int, SearchBarHit *) = dlsym(library, "searchbar_index_search_metadata");
    int (*full)(SearchBarIndex *, const char *, int, int, SearchBarHit *) = dlsym(library, "searchbar_index_search");
    SearchBarSearch *(*open_search)(SearchBarIndex *, const char *, int, int) = dlsym(library, "searchbar_search_open");
    int (*search_metadata)(SearchBarSearch *) = dlsym(library, "searchbar_search_metadata");
    int (*complete)(SearchBarSearch *) = dlsym(library, "searchbar_search_complete");
    int (*finished)(SearchBarSearch *) = dlsym(library, "searchbar_search_finished");
    int (*read_result)(SearchBarSearch *, int, SearchBarResult *) = dlsym(library, "searchbar_search_read");
    void (*close_search)(SearchBarSearch *) = dlsym(library, "searchbar_search_close");
    SearchBarIndex *index = open_index(argv[2]);
    if (!index) return 1;
    if (upgrade && !upgrade(index)) { fprintf(stderr, "%s\n", error(index)); return 1; }
    double start = now();
    int prepared = prepare ? prepare(index) : 0;
    fprintf(stderr, "Snapshot prepare %.3fs (%d)\n", now() - start, prepared);
    SearchBarHit *hits = calloc(48, sizeof(*hits));
    if (!hits) return 1;
    puts("[");
    for (int q = 3; q < argc; ++q) {
        double first[100], total[100]; int count = 0;
        for (int repetition = -10; repetition < 100; ++repetition) {
            start = now(); double first_end;
            if (open_search) {
                SearchBarSearch *search = open_search(index, argv[q], -1, 48);
                if (!search) return 1;
                count = search_metadata(search); SearchBarResult result;
                for (int i = 0; i < count; ++i) if (!read_result(search, i, &result)) return 1;
                first_end = now();
                if (!finished(search)) {
                    count = complete(search);
                    for (int i = 0; i < count; ++i) if (!read_result(search, i, &result)) return 1;
                }
                close_search(search);
            } else {
                count = metadata(index, argv[q], -1, 48, hits); first_end = now();
                if (count < 48 || hits[count - 1].score <= 300) count = full(index, argv[q], -1, 48, hits);
            }
            if (count < 0) { fprintf(stderr, "%s\n", error(index)); return 1; }
            if (repetition >= 0) {
                first[repetition] = (first_end - start) * 1000;
                total[repetition] = (now() - start) * 1000;
            }
        }
        qsort(first, 100, sizeof(double), compare); qsort(total, 100, sizeof(double), compare);
        fputs("{\"query\":", stdout); json_string(argv[q]);
        printf(",\"hits\":%d,\"metadata_p50_ms\":%.6f,\"metadata_p95_ms\":%.6f,"
               "\"complete_p50_ms\":%.6f,\"complete_p95_ms\":%.6f}%s\n",
               count, first[49], first[94], total[49], total[94], q + 1 == argc ? "" : ",");
    }
    puts("]"); free(hits); close_index(index); dlclose(library); return 0;
}
