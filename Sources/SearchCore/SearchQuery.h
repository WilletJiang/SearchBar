#ifndef SEARCHBAR_SEARCH_QUERY_H
#define SEARCHBAR_SEARCH_QUERY_H

#define SEARCHBAR_QUERY_BYTES 2048
#define SEARCHBAR_MAX_TERMS 24

typedef struct {
    char text[SEARCHBAR_QUERY_BYTES];
    char *terms[SEARCHBAR_MAX_TERMS];
    int count;
    char prefix[SEARCHBAR_QUERY_BYTES];
} SearchQuery;

int searchbar_query_parse(SearchQuery *query, const char *text);
/* Match expressions are owned by SQLite; release with sqlite3_free. */
char *searchbar_query_match(SearchQuery *query, int body, int fuzzy);
char *searchbar_query_body_expression(SearchQuery *query, int positional);
double searchbar_query_score(SearchQuery *query, const char *name, const char *path,
                         const char *aliases, int fuzzy);
int searchbar_query_body_matches(SearchQuery *query, const char *body);

#endif
