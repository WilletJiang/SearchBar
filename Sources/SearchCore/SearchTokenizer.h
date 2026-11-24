#ifndef SEARCHBAR_SEARCH_TOKENIZER_H
#define SEARCHBAR_SEARCH_TOKENIZER_H

#include <sqlite3.h>
#include <stdint.h>

/* Internal helpers. Folded strings are malloc-owned by the caller. */
char *searchbar_fold_text(const char *text);
uint32_t searchbar_codepoint(const char *text, int bytes, int *width);
int searchbar_token_character(uint32_t point);
int searchbar_cjk_character(uint32_t point);
int searchbar_register_tokenizers(sqlite3 *database);
int searchbar_tokenize_body(const char *text, int bytes, void *context,
                       int (*emit)(void *, int, const char *, int, int, int));
int searchbar_register_offsets(sqlite3 *database);
int searchbar_excerpt_offset(const void *offsets, int bytes, int position);
void searchbar_excerpt_text(char out[512], const char *text, int bytes, int byte_offset);

#endif
