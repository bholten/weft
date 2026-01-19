#ifndef WEFT_TREE_SITTER_HL_H
#define WEFT_TREE_SITTER_HL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct TSLanguage TSLanguage;
typedef struct TSParser TSParser;
typedef struct TSTree TSTree;
typedef struct TSQuery TSQuery;
typedef struct Buffer Buffer;

// Highlight capture - a single syntax highlight match
typedef struct {
  size_t start;        // Start byte offset
  size_t end;          // End byte offset
  const char *capture; // Capture name (e.g., "keyword", "string")
} TSHighlight;

// Highlight result - list of all highlights
typedef struct {
  TSHighlight *highlights;
  size_t count;
  size_t capacity;
} TSHighlightResult;

// Tree-sitter highlighter for a specific language
typedef struct TSHighlighter {
  TSParser *parser;
  TSTree *tree;
  TSQuery *query;
  const TSLanguage *language;
  char *language_name;
  char *query_source; // Keep query source for error reporting
} TSHighlighter;

// === Lifecycle ===

// Create a new highlighter for a language
// query_source is the contents of a highlights.scm file
TSHighlighter *ts_hl_new(const TSLanguage *language, const char *language_name,
                         const char *query_source);

// Free the highlighter
void ts_hl_free(TSHighlighter *hl);

// === Parsing ===

// Parse a document (or reparse if tree exists)
// Returns true on success
bool ts_hl_parse(TSHighlighter *hl, const char *source, size_t source_len);

// === Highlighting ===

// Run highlight query and get all matches
// Caller must free result with ts_hl_result_free()
TSHighlightResult *ts_hl_highlight(TSHighlighter *hl);

// Free a highlight result
void ts_hl_result_free(TSHighlightResult *result);

// === Convenience ===

// Apply highlights to a buffer as annotations in the "style" layer
// Maps capture names to colors using a default color scheme
void ts_hl_apply_to_buffer(TSHighlighter *hl, Buffer *buffer,
                           const char *script_path);

// === Built-in Languages ===

// Get the Lcl language highlighter (creates on first call, cached)
// Returns NULL if highlights.scm cannot be loaded
TSHighlighter *ts_hl_get_lcl(const char *highlights_path);

// Free cached highlighters (call at shutdown)
void ts_hl_cleanup(void);

#endif // WEFT_TREE_SITTER_HL_H
