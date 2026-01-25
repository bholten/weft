#include "lcl_host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <lcl-regex.h>
#include <lcl.h>

#include "annot_layer.h"
#include "annot_persist.h"
#include "annot_store.h"
#include "buffer.h"
#include "document.h"
#include "editor.h"
#include "interaction.h"
#include "layer.h"
#include "layer_stack.h"
#include "layout.h"
#include "pane.h"
#include "patch.h"
#include "transform.h"
#include "tree_sitter_hl.h"
#include "tree_sitter_layer.h"
#include "ts_lang_registry.h"

// Embedded stdlib (generated at build time)
#include "embedded_stdlib.h"

// Conditional includes for embedded tree-sitter parsers
#ifdef WEFT_EMBED_TS_LCL
#include <tree_sitter/tree-sitter-lcl.h>
#endif
#ifdef WEFT_EMBED_TS_C
#include <tree_sitter/tree-sitter-c.h>
#endif
#ifdef WEFT_EMBED_TS_JSON
#include <tree-sitter-json.h>
#endif
#ifdef WEFT_EMBED_TS_BASH
#include <tree_sitter/tree-sitter-bash.h>
#endif
#ifdef WEFT_EMBED_TS_JAVASCRIPT
#include <tree_sitter/tree-sitter-javascript.h>
#endif

#include <SDL3/SDL.h>

#define INITIAL_LAYER_CAP 8
#define INITIAL_HANDLER_CAP 8

// Forward declarations
static int plumb_scan_buffer(Buffer *buf);
static void recompute_layout_bounds(LclHost *host);
static bool is_repl_buffer(const Buffer *buf);
static Buffer *get_effective_doc_buffer(LclHost *host);
static bool close_focused_pane(LclHost *host);
static bool kill_focused_pane(LclHost *host);
static Pane *find_pane_for_buffer(LclHost *host, Buffer *buffer);
static bool get_effective_selection_context(LclHost *host, Editor **out_editor,
                                            Buffer **out_buffer);

// === Internal: Weft Bindings ===

// weft::version -> returns version string
static int c_weft_version(lcl_interp *interp, int argc, lcl_value **argv,
                          lcl_value **out) {
  (void)interp;
  (void)argc;
  (void)argv;
  *out = lcl_string_new("0.1.0");
  return LCL_RC_OK;
}

// weft::doc::length -> returns document length in bytes
static int c_doc_length(lcl_interp *interp, int argc, lcl_value **argv,
                        lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  size_t len = document_length(buffer->document);
  *out = lcl_int_new((long)len);
  return LCL_RC_OK;
}

// weft::doc::line-count -> returns number of lines
static int c_doc_line_count(lcl_interp *interp, int argc, lcl_value **argv,
                            lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  size_t count = document_line_count(buffer->document);
  *out = lcl_int_new((long)count);
  return LCL_RC_OK;
}

// weft::doc::text start len -> returns text from document
static int c_doc_text(lcl_interp *interp, int argc, lcl_value **argv,
                      lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp, "doc::text requires start and length");
    return LCL_RC_ERR;
  }

  long start, len;
  if (lcl_value_to_int(argv[0], &start) != LCL_OK || start < 0) {
    lcl_set_error(interp, "doc::text: invalid start position");
    return LCL_RC_ERR;
  }
  if (lcl_value_to_int(argv[1], &len) != LCL_OK || len < 0) {
    lcl_set_error(interp, "doc::text: invalid length");
    return LCL_RC_ERR;
  }

  Document *doc = buffer->document;
  size_t doc_len = document_length(doc);

  // Clamp to document bounds
  if ((size_t)start >= doc_len) {
    *out = lcl_string_new("");
    return LCL_RC_OK;
  }
  if ((size_t)(start + len) > doc_len) {
    len = (long)(doc_len - (size_t)start);
  }

  // Allocate buffer and get text
  char *buf = malloc((size_t)len + 1);
  if (!buf) {
    lcl_set_error(interp, "out of memory");
    return LCL_RC_ERR;
  }

  size_t got = document_get_text(doc, (size_t)start, buf, (size_t)len);
  buf[got] = '\0';

  *out = lcl_string_new(buf);
  free(buf);
  return LCL_RC_OK;
}

// weft::doc::line-start n -> returns byte offset of line start
static int c_doc_line_start(lcl_interp *interp, int argc, lcl_value **argv,
                            lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "doc::line-start requires line number");
    return LCL_RC_ERR;
  }

  long line;
  if (lcl_value_to_int(argv[0], &line) != LCL_OK || line < 0) {
    lcl_set_error(interp, "doc::line-start: invalid line number");
    return LCL_RC_ERR;
  }

  Document *doc = buffer->document;
  size_t line_count = document_line_count(doc);

  if ((size_t)line >= line_count) {
    *out = lcl_int_new((long)document_length(doc));
    return LCL_RC_OK;
  }

  size_t pos = document_line_start(doc, (size_t)line);
  *out = lcl_int_new((long)pos);
  return LCL_RC_OK;
}

// weft::doc::line-end n -> returns byte offset of line end (before newline)
static int c_doc_line_end(lcl_interp *interp, int argc, lcl_value **argv,
                          lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "doc::line-end requires line number");
    return LCL_RC_ERR;
  }

  long line;
  if (lcl_value_to_int(argv[0], &line) != LCL_OK || line < 0) {
    lcl_set_error(interp, "doc::line-end: invalid line number");
    return LCL_RC_ERR;
  }

  Document *doc = buffer->document;
  size_t line_count = document_line_count(doc);

  if ((size_t)line >= line_count) {
    *out = lcl_int_new((long)document_length(doc));
    return LCL_RC_OK;
  }

  size_t pos = document_line_end(doc, (size_t)line);
  *out = lcl_int_new((long)pos);
  return LCL_RC_OK;
}

// === Internal: Annotation Bindings ===

// weft::annot::add start end layer ?key value ...? -> returns annotation ID
static int c_annot_add(lcl_interp *interp, int argc, lcl_value **argv,
                       lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);

  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (argc < 3) {
    lcl_set_error(interp, "annot::add requires start, end, layer");
    return LCL_RC_ERR;
  }

  long start;
  long end;

  if (lcl_value_to_int(argv[0], &start) != LCL_OK || start < 0) {
    lcl_set_error(interp, "annot::add: invalid start position");
    return LCL_RC_ERR;
  }

  if (lcl_value_to_int(argv[1], &end) != LCL_OK || end < 0) {
    lcl_set_error(interp, "annot::add: invalid end position");
    return LCL_RC_ERR;
  }

  const char *layer = lcl_value_to_string(argv[2]);

  if (!layer) {
    lcl_set_error(interp, "annot::add: layer must be a string");
    return LCL_RC_ERR;
  }

  size_t user_meta_count = 0;

  if (argc > 3) {
    if ((argc - 3) % 2 != 0) {
      lcl_set_error(interp, "annot::add: key-value pairs must be even");
      return LCL_RC_ERR;
    }
    user_meta_count = (size_t)(argc - 3) / 2;
  }

  size_t prov_count = host->current_script_path ? 1 : 0;
  size_t meta_count = user_meta_count + prov_count;

  const char **keys = NULL;
  const char **values = NULL;

  if (meta_count > 0) {
    keys = malloc(meta_count * sizeof(const char *));
    values = malloc(meta_count * sizeof(const char *));

    if (!keys || !values) {
      free(keys);
      free(values);
      lcl_set_error(interp, "out of memory");
      return LCL_RC_ERR;
    }

    for (size_t i = 0; i < user_meta_count; i++) {
      keys[i] = lcl_value_to_string(argv[3 + i * 2]);
      values[i] = lcl_value_to_string(argv[3 + i * 2 + 1]);

      if (!keys[i] || !values[i]) {
        free(keys);
        free(values);
        lcl_set_error(interp, "annot::add: keys and values must be strings");
        return LCL_RC_ERR;
      }
    }

    // Add provenance metadata if available
    if (host->current_script_path) {
      keys[user_meta_count] = "prov:script";
      values[user_meta_count] = host->current_script_path;
    }
  }

  uint32_t id = annot_store_add(buffer->annots, (size_t)start, (size_t)end,
                                layer, keys, values, meta_count);

  free(keys);
  free(values);

  *out = lcl_int_new((long)id);

  return LCL_RC_OK;
}

// weft::annot::remove id -> removes annotation, returns true/false
static int c_annot_remove(lcl_interp *interp, int argc, lcl_value **argv,
                          lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "annot::remove requires id");
    return LCL_RC_ERR;
  }

  long id;
  if (lcl_value_to_int(argv[0], &id) != LCL_OK || id < 0) {
    lcl_set_error(interp, "annot::remove: invalid id");
    return LCL_RC_ERR;
  }

  bool removed = annot_store_remove(buffer->annots, (uint32_t)id);
  *out = lcl_int_new(removed ? 1 : 0);
  return LCL_RC_OK;
}

// weft::annot::at pos ?layer? -> returns list of annotation IDs at position
static int c_annot_at(lcl_interp *interp, int argc, lcl_value **argv,
                      lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "annot::at requires position");
    return LCL_RC_ERR;
  }

  long pos;
  if (lcl_value_to_int(argv[0], &pos) != LCL_OK || pos < 0) {
    lcl_set_error(interp, "annot::at: invalid position");
    return LCL_RC_ERR;
  }

  const char *layer = NULL;
  if (argc > 1) {
    layer = lcl_value_to_string(argv[1]);
  }

  AnnotQuery query = {0};
  annot_store_at(buffer->annots, (size_t)pos, layer, &query);

  // Build result list
  lcl_value *list = lcl_list_new();
  for (size_t i = 0; i < query.count; i++) {
    lcl_list_push(&list, lcl_int_new((long)query.ids[i]));
  }
  annot_query_free(&query);

  *out = list;
  return LCL_RC_OK;
}

// weft::annot::in-range start end ?layer? -> returns list of annotation IDs
static int c_annot_in_range(lcl_interp *interp, int argc, lcl_value **argv,
                            lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp, "annot::in-range requires start and end");
    return LCL_RC_ERR;
  }

  long start, end;
  if (lcl_value_to_int(argv[0], &start) != LCL_OK || start < 0) {
    lcl_set_error(interp, "annot::in-range: invalid start");
    return LCL_RC_ERR;
  }
  if (lcl_value_to_int(argv[1], &end) != LCL_OK || end < 0) {
    lcl_set_error(interp, "annot::in-range: invalid end");
    return LCL_RC_ERR;
  }

  const char *layer = NULL;
  if (argc > 2) {
    layer = lcl_value_to_string(argv[2]);
  }

  AnnotQuery query = {0};
  annot_store_in_range(buffer->annots, (size_t)start, (size_t)end, layer,
                       &query);

  // Build result list
  lcl_value *list = lcl_list_new();
  for (size_t i = 0; i < query.count; i++) {
    lcl_list_push(&list, lcl_int_new((long)query.ids[i]));
  }
  annot_query_free(&query);

  *out = list;
  return LCL_RC_OK;
}

// weft::annot::get id -> returns dict with annotation info
static int c_annot_get(lcl_interp *interp, int argc, lcl_value **argv,
                       lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "annot::get requires id");
    return LCL_RC_ERR;
  }

  long id;
  if (lcl_value_to_int(argv[0], &id) != LCL_OK || id < 0) {
    lcl_set_error(interp, "annot::get: invalid id");
    return LCL_RC_ERR;
  }

  const AnnotRecord *rec = annot_store_get(buffer->annots, (uint32_t)id);
  if (!rec) {
    *out = lcl_list_new();
    return LCL_RC_OK;
  }

  // Get the anchor positions
  AnnotStore *store = buffer->annots;
  size_t start_pos = 0, end_pos = 0;
  for (size_t i = 0; i < store->anchor_count; i++) {
    if (store->anchors[i].id == rec->start_anchor) {
      start_pos = store->anchors[i].pos;
    }
    if (store->anchors[i].id == rec->end_anchor) {
      end_pos = store->anchors[i].pos;
    }
  }

  // Build result dict (as list of key-value pairs for now)
  lcl_value *dict = lcl_list_new();
  lcl_list_push(&dict, lcl_string_new("id"));
  lcl_list_push(&dict, lcl_int_new((long)rec->id));
  lcl_list_push(&dict, lcl_string_new("start"));
  lcl_list_push(&dict, lcl_int_new((long)start_pos));
  lcl_list_push(&dict, lcl_string_new("end"));
  lcl_list_push(&dict, lcl_int_new((long)end_pos));
  lcl_list_push(&dict, lcl_string_new("layer"));
  lcl_list_push(&dict, lcl_string_new(rec->layer ? rec->layer : ""));

  // Add metadata
  for (size_t i = 0; i < rec->meta_count; i++) {
    lcl_list_push(&dict, lcl_string_new(rec->keys[i]));
    lcl_list_push(&dict, lcl_string_new(rec->values[i]));
  }

  *out = dict;
  return LCL_RC_OK;
}

// weft::annot::clear-layer layer -> clears all annotations in layer
static int c_annot_clear_layer(lcl_interp *interp, int argc, lcl_value **argv,
                               lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "annot::clear-layer requires layer name");
    return LCL_RC_ERR;
  }

  const char *layer = lcl_value_to_string(argv[0]);
  if (!layer) {
    lcl_set_error(interp, "annot::clear-layer: layer must be a string");
    return LCL_RC_ERR;
  }

  annot_store_clear_layer(buffer->annots, layer);
  *out = lcl_list_new();
  return LCL_RC_OK;
}

// === Internal: Annotation Editor ===

// Helper struct for parsing annotation blocks
typedef struct {
  uint32_t id;       // Annotation ID (0 = new)
  bool is_new;       // true if id was "new"
  size_t start;      // Start position
  size_t end;        // End position
  bool has_start;    // true if start was explicitly provided
  bool has_end;      // true if end was explicitly provided
  char *layer;       // Layer name (owned)
  char **keys;       // Metadata keys (owned)
  char **values;     // Metadata values (owned)
  size_t meta_count; // Number of key/value pairs
  bool valid;        // Parsing succeeded
} AnnotEditBlock;

// Free an AnnotEditBlock
static void annot_edit_block_free(AnnotEditBlock *block) {
  if (!block) {
    return;
  }
  free(block->layer);
  for (size_t i = 0; i < block->meta_count; i++) {
    free(block->keys[i]);
    free(block->values[i]);
  }
  free(block->keys);
  free(block->values);
  block->layer = NULL;
  block->keys = NULL;
  block->values = NULL;
  block->meta_count = 0;
}

// Helper to escape a string for output (handles quotes)
static char *escape_string(const char *s) {
  if (!s) {
    return strdup("");
  }
  size_t len = strlen(s);
  size_t escaped_len = len;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '"' || s[i] == '\\') {
      escaped_len++;
    }
  }
  char *result = malloc(escaped_len + 1);
  if (!result) {
    return NULL;
  }
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '"' || s[i] == '\\') {
      result[j++] = '\\';
    }
    result[j++] = s[i];
  }
  result[j] = '\0';
  return result;
}

// Helper to unescape a quoted string (modifies in place, returns new length)
static size_t unescape_string(char *s, size_t len) {
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '\\' && i + 1 < len) {
      i++;
    }
    s[j++] = s[i];
  }
  s[j] = '\0';
  return j;
}

// Helper to clear annotation editor state
static void annot_edit_clear(LclHost *host) {
  if (!host) {
    return;
  }
  host->annot_edit.target_buffer = NULL;
  free(host->annot_edit.original_ids);
  host->annot_edit.original_ids = NULL;
  host->annot_edit.original_count = 0;
  host->annot_edit.doc_rev = 0;
  host->annot_edit.query_start = 0;
  host->annot_edit.query_end = 0;
  host->annot_edit.is_range = false;
  host->annot_edit.active = false;
}

// Generate annotation editor content for a list of annotation IDs
static char *generate_annot_editor_content(Buffer *buffer, uint32_t *ids,
                                           size_t count, size_t query_start,
                                           size_t query_end, bool is_range) {
  AnnotStore *store = buffer->annots;

  // Calculate buffer size - start with generous estimate
  size_t cap = 4096 + count * 1024;
  char *output = malloc(cap);
  if (!output) {
    return NULL;
  }
  size_t len = 0;

  // Header
  const char *path = buffer->file_path ? buffer->file_path : "(untitled)";
  len += snprintf(output + len, cap - len, ";; Annotation Editor - B%u (%s)\n",
                  buffer->id, path);

  if (is_range) {
    len += snprintf(output + len, cap - len, ";; Scope: range [%zu, %zu)\n",
                    query_start, query_end);
  } else {
    len += snprintf(output + len, cap - len, ";; Scope: position %zu\n",
                    query_start);
  }

  uint64_t rev = annot_store_get_doc_rev(store);
  len += snprintf(output + len, cap - len, ";; Revision: %lu\n",
                  (unsigned long)rev);
  len += snprintf(output + len, cap - len,
                  "[weft::annot::apply-edit]  [weft::annot::cancel-edit]\n\n");

  // Output each annotation as a block
  for (size_t i = 0; i < count; i++) {
    uint32_t id = ids[i];
    const AnnotRecord *rec = annot_store_get(store, id);
    if (!rec) {
      continue;
    }

    // Get span positions
    size_t start_pos = 0, end_pos = 0;
    annot_store_get_span(store, id, &start_pos, &end_pos);

    // Ensure we have space
    size_t needed = len + 512 + (rec->meta_count * 256);
    if (needed > cap) {
      cap = needed * 2;
      char *new_output = realloc(output, cap);
      if (!new_output) {
        free(output);
        return NULL;
      }
      output = new_output;
    }

    // Escape layer name
    char *escaped_layer = escape_string(rec->layer);
    if (!escaped_layer) {
      free(output);
      return NULL;
    }

    // Don't include start/end in output - positions come from the store
    // and are implicit in the scope
    len += snprintf(output + len, cap - len, "{id %u layer \"%s\"", id,
                    escaped_layer);
    free(escaped_layer);

    (void)start_pos; // Positions are available but not displayed
    (void)end_pos;

    // Metadata (skip prov:* keys, they're auto-added)
    for (size_t j = 0; j < rec->meta_count; j++) {
      if (strncmp(rec->keys[j], "prov:", 5) == 0) {
        continue;
      }

      // Ensure space
      if (len + 256 > cap) {
        cap *= 2;
        char *new_output = realloc(output, cap);
        if (!new_output) {
          free(output);
          return NULL;
        }
        output = new_output;
      }

      char *escaped_val = escape_string(rec->values[j]);
      if (!escaped_val) {
        free(output);
        return NULL;
      }

      len += snprintf(output + len, cap - len, "\n  %s \"%s\"", rec->keys[j],
                      escaped_val);
      free(escaped_val);
    }

    len += snprintf(output + len, cap - len, "}\n\n");
  }

  // Footer with help text
  len += snprintf(output + len, cap - len,
                  ";; To add: {id new layer \"name\" key \"val\"}\n"
                  ";; To delete: remove the block entirely\n"
                  ";; New annotations use the scope range shown above\n");

  return output;
}

// Parse a single annotation block from text
// Returns true on success
static bool parse_annot_block(const char *text, size_t text_len,
                              AnnotEditBlock *out) {
  memset(out, 0, sizeof(*out));
  out->valid = false;

  // Skip leading whitespace and find '{'
  size_t pos = 0;
  while (pos < text_len && (text[pos] == ' ' || text[pos] == '\t' ||
                            text[pos] == '\n' || text[pos] == '\r')) {
    pos++;
  }

  if (pos >= text_len || text[pos] != '{') {
    return false;
  }
  pos++; // Skip '{'

  // Temporary storage for key-value pairs
  char *keys[64];
  char *values[64];
  size_t kv_count = 0;

  bool has_id = false, has_start = false, has_end = false, has_layer = false;

  // Parse tokens until '}'
  while (pos < text_len && text[pos] != '}') {
    // Skip whitespace
    while (pos < text_len && (text[pos] == ' ' || text[pos] == '\t' ||
                              text[pos] == '\n' || text[pos] == '\r')) {
      pos++;
    }

    if (pos >= text_len || text[pos] == '}') {
      break;
    }

    // Read key (non-quoted token)
    size_t key_start = pos;
    while (pos < text_len && text[pos] != ' ' && text[pos] != '\t' &&
           text[pos] != '\n' && text[pos] != '}' && text[pos] != '"') {
      pos++;
    }
    size_t key_len = pos - key_start;

    if (key_len == 0) {
      // Maybe hit a quote or closing brace
      if (text[pos] == '}') {
        break;
      }
      pos++;
      continue;
    }

    char *key = malloc(key_len + 1);
    if (!key) {
      goto cleanup_error;
    }
    memcpy(key, text + key_start, key_len);
    key[key_len] = '\0';

    // Skip whitespace
    while (pos < text_len && (text[pos] == ' ' || text[pos] == '\t' ||
                              text[pos] == '\n' || text[pos] == '\r')) {
      pos++;
    }

    // Read value (either quoted string or plain token)
    char *value = NULL;

    if (pos < text_len && text[pos] == '"') {
      // Quoted string
      pos++; // Skip opening quote
      size_t val_start = pos;

      // Find closing quote, handling escapes
      while (pos < text_len) {
        if (text[pos] == '\\' && pos + 1 < text_len) {
          pos += 2;
        } else if (text[pos] == '"') {
          break;
        } else {
          pos++;
        }
      }

      size_t val_len = pos - val_start;
      value = malloc(val_len + 1);
      if (!value) {
        free(key);
        goto cleanup_error;
      }
      memcpy(value, text + val_start, val_len);
      value[val_len] = '\0';
      unescape_string(value, val_len);

      if (pos < text_len && text[pos] == '"') {
        pos++; // Skip closing quote
      }
    } else {
      // Plain token
      size_t val_start = pos;
      while (pos < text_len && text[pos] != ' ' && text[pos] != '\t' &&
             text[pos] != '\n' && text[pos] != '}') {
        pos++;
      }
      size_t val_len = pos - val_start;

      value = malloc(val_len + 1);
      if (!value) {
        free(key);
        goto cleanup_error;
      }
      memcpy(value, text + val_start, val_len);
      value[val_len] = '\0';
    }

    // Handle special keys
    if (strcmp(key, "id") == 0) {
      if (strcmp(value, "new") == 0) {
        out->is_new = true;
        out->id = 0;
      } else {
        out->id = (uint32_t)strtoul(value, NULL, 10);
        out->is_new = false;
      }
      has_id = true;
      free(key);
      free(value);
    } else if (strcmp(key, "start") == 0) {
      out->start = (size_t)strtoul(value, NULL, 10);
      out->has_start = true;
      has_start = true;
      free(key);
      free(value);
    } else if (strcmp(key, "end") == 0) {
      out->end = (size_t)strtoul(value, NULL, 10);
      out->has_end = true;
      has_end = true;
      free(key);
      free(value);
    } else if (strcmp(key, "layer") == 0) {
      out->layer = value;
      has_layer = true;
      free(key);
    } else {
      // Regular metadata key-value
      if (kv_count < 64) {
        keys[kv_count] = key;
        values[kv_count] = value;
        kv_count++;
      } else {
        free(key);
        free(value);
      }
    }
  }

  // Validate required fields (start/end are now optional)
  if (!has_id || !has_layer) {
    goto cleanup_error;
  }

  (void)has_start; // Now optional - used only for explicit overrides
  (void)has_end;

  // Copy metadata to output
  if (kv_count > 0) {
    out->keys = malloc(kv_count * sizeof(char *));
    out->values = malloc(kv_count * sizeof(char *));
    if (!out->keys || !out->values) {
      goto cleanup_error;
    }
    for (size_t i = 0; i < kv_count; i++) {
      out->keys[i] = keys[i];
      out->values[i] = values[i];
    }
    out->meta_count = kv_count;
  }

  out->valid = true;
  return true;

cleanup_error:
  free(out->layer);
  out->layer = NULL;
  for (size_t i = 0; i < kv_count; i++) {
    free(keys[i]);
    free(values[i]);
  }
  return false;
}

// Parse all annotation blocks from editor buffer content
// Returns array of AnnotEditBlock, count in out_count
// Caller must free each block with annot_edit_block_free and free the array
static AnnotEditBlock *
parse_annot_blocks(const char *content, size_t content_len, size_t *out_count) {
  *out_count = 0;

  // Count potential blocks
  size_t block_cap = 16;
  AnnotEditBlock *blocks = calloc(block_cap, sizeof(AnnotEditBlock));
  if (!blocks) {
    return NULL;
  }

  size_t pos = 0;
  while (pos < content_len) {
    // Skip to next '{'
    while (pos < content_len && content[pos] != '{') {
      // Skip comment lines
      if (content[pos] == ';' && pos + 1 < content_len &&
          content[pos + 1] == ';') {
        while (pos < content_len && content[pos] != '\n') {
          pos++;
        }
      }
      pos++;
    }

    if (pos >= content_len) {
      break;
    }

    // Find matching '}'
    size_t block_start = pos;
    int depth = 1;
    pos++; // Skip opening '{'
    bool in_string = false;

    while (pos < content_len && depth > 0) {
      if (content[pos] == '\\' && pos + 1 < content_len && in_string) {
        pos += 2;
        continue;
      }
      if (content[pos] == '"') {
        in_string = !in_string;
      } else if (!in_string) {
        if (content[pos] == '{') {
          depth++;
        } else if (content[pos] == '}') {
          depth--;
        }
      }
      pos++;
    }

    if (depth != 0) {
      // Unmatched braces
      continue;
    }

    size_t block_len = pos - block_start;

    // Parse this block
    AnnotEditBlock block;
    if (parse_annot_block(content + block_start, block_len, &block)) {
      // Add to array
      if (*out_count >= block_cap) {
        block_cap *= 2;
        AnnotEditBlock *new_blocks =
            realloc(blocks, block_cap * sizeof(AnnotEditBlock));
        if (!new_blocks) {
          annot_edit_block_free(&block);
          for (size_t i = 0; i < *out_count; i++) {
            annot_edit_block_free(&blocks[i]);
          }
          free(blocks);
          return NULL;
        }
        blocks = new_blocks;
      }
      blocks[(*out_count)++] = block;
    }
  }

  return blocks;
}

// weft::annot::edit-at ?pos? -> open annotation editor for annotations at
// position
static int c_annot_edit_at(lcl_interp *interp, int argc, lcl_value **argv,
                           lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (!host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  // Check if already editing - handle gracefully
  if (host->annot_edit.active) {
    // Check if *annot-edit* buffer still exists
    Buffer *edit_buf =
        buffer_manager_find_by_path(host->buffer_manager, "*annot-edit*");
    if (edit_buf) {
      // Buffer exists - check if it has a pane
      Pane *existing_pane = find_pane_for_buffer(host, edit_buf);
      if (existing_pane) {
        // Already visible - just focus it
        if (host->focused_pane_ptr) {
          *host->focused_pane_ptr = existing_pane;
        }
        *out = lcl_int_new((long)edit_buf->id);
        return LCL_RC_OK;
      } else {
        // Hidden - re-summon it by creating a new pane
        Pane *new_pane =
            pane_new(edit_buf, host->char_width, host->line_height);
        if (new_pane) {
          lcl_host_apply_viewlayers(host, new_pane);
          LayoutNode *root = *host->layout_root_ptr;
          LayoutNode *new_split = layout_split_horizontal(root, new_pane);
          if (new_split && !new_split->parent) {
            *host->layout_root_ptr = new_split;
          }
          recompute_layout_bounds(host);
          if (host->focused_pane_ptr) {
            *host->focused_pane_ptr = new_pane;
          }
        }
        *out = lcl_int_new((long)edit_buf->id);
        return LCL_RC_OK;
      }
    } else {
      // Buffer was killed but active flag not cleared - reset state
      annot_edit_clear(host);
      // Fall through to create new editor
    }
  }

  // Get position/range: use selection if present, else cursor or explicit arg
  size_t query_start = 0;
  size_t query_end = 0;
  bool is_range = false;

  if (argc >= 1) {
    // Explicit position given
    long arg_pos;
    if (lcl_value_to_int(argv[0], &arg_pos) != LCL_OK || arg_pos < 0) {
      lcl_set_error(interp, "annot::edit-at: invalid position");
      return LCL_RC_ERR;
    }
    query_start = (size_t)arg_pos;
    query_end = query_start;
  } else {
    // No args - use selection if present, else cursor
    Editor *ctx_editor = NULL;
    Buffer *ctx_buffer = NULL;
    if (get_effective_selection_context(host, &ctx_editor, &ctx_buffer) &&
        ctx_buffer == buffer) {
      // Use selection from the effective context
      size_t sel_start, sel_end;
      editor_get_selection(ctx_editor, &sel_start, &sel_end);
      if (sel_start != sel_end) {
        // There's a selection - use it as a range
        query_start = sel_start;
        query_end = sel_end;
        is_range = true;
      } else {
        // No selection - use cursor
        query_start = editor_get_cursor_pos(ctx_editor);
        query_end = query_start;
      }
    } else if (host->current_editor) {
      // Fallback to current editor's cursor
      query_start = editor_get_cursor_pos(host->current_editor);
      query_end = query_start;
    }
  }

  // Query annotations at position or in range
  AnnotQuery query = {0};
  if (is_range) {
    annot_store_in_range(buffer->annots, query_start, query_end, NULL, &query);
  } else {
    annot_store_at(buffer->annots, query_start, NULL, &query);
  }

  // Store original IDs
  uint32_t *original_ids = NULL;
  if (query.count > 0) {
    original_ids = malloc(query.count * sizeof(uint32_t));
    if (!original_ids) {
      annot_query_free(&query);
      lcl_set_error(interp, "out of memory");
      return LCL_RC_ERR;
    }
    memcpy(original_ids, query.ids, query.count * sizeof(uint32_t));
  }
  size_t original_count = query.count;

  // Generate editor content
  char *content = generate_annot_editor_content(
      buffer, query.ids, query.count, query_start, query_end, is_range);
  annot_query_free(&query);

  if (!content) {
    free(original_ids);
    lcl_set_error(interp, "failed to generate editor content");
    return LCL_RC_ERR;
  }

  // Create editor buffer
  Buffer *edit_buffer = buffer_manager_create(host->buffer_manager);
  if (!edit_buffer) {
    free(original_ids);
    free(content);
    lcl_set_error(interp, "failed to create buffer");
    return LCL_RC_ERR;
  }

  document_insert(edit_buffer->document, 0, content, strlen(content));
  free(content);

  edit_buffer->file_path = strdup("*annot-edit*");

  // Create pane
  Pane *new_pane = pane_new(edit_buffer, host->char_width, host->line_height);
  if (!new_pane) {
    buffer_manager_close(host->buffer_manager, edit_buffer);
    free(original_ids);
    lcl_set_error(interp, "failed to create pane");
    return LCL_RC_ERR;
  }

  // Apply Lcl syntax highlighting to the annotation editor
  ViewLayer *lcl_layer = tree_sitter_layer_new_lcl(0);
  if (lcl_layer && new_pane->layer_stack) {
    layer_stack_add(new_pane->layer_stack, lcl_layer);
  }

  // Split to show new pane
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }

  // Store state
  host->annot_edit.target_buffer = buffer;
  host->annot_edit.original_ids = original_ids;
  host->annot_edit.original_count = original_count;
  host->annot_edit.doc_rev = annot_store_get_doc_rev(buffer->annots);
  host->annot_edit.query_start = query_start;
  host->annot_edit.query_end = query_end;
  host->annot_edit.is_range = is_range;
  host->annot_edit.active = true;

  *out = lcl_int_new((long)original_count);
  return LCL_RC_OK;
}

// weft::annot::edit-range start end -> open annotation editor for range
static int c_annot_edit_range(lcl_interp *interp, int argc, lcl_value **argv,
                              lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (!host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  // Check if already editing - handle gracefully
  if (host->annot_edit.active) {
    // Check if *annot-edit* buffer still exists
    Buffer *edit_buf =
        buffer_manager_find_by_path(host->buffer_manager, "*annot-edit*");
    if (edit_buf) {
      // Buffer exists - check if it has a pane
      Pane *existing_pane = find_pane_for_buffer(host, edit_buf);
      if (existing_pane) {
        // Already visible - just focus it
        if (host->focused_pane_ptr) {
          *host->focused_pane_ptr = existing_pane;
        }
        *out = lcl_int_new((long)edit_buf->id);
        return LCL_RC_OK;
      } else {
        // Hidden - re-summon it by creating a new pane
        Pane *new_pane =
            pane_new(edit_buf, host->char_width, host->line_height);
        if (new_pane) {
          lcl_host_apply_viewlayers(host, new_pane);
          LayoutNode *root = *host->layout_root_ptr;
          LayoutNode *new_split = layout_split_horizontal(root, new_pane);
          if (new_split && !new_split->parent) {
            *host->layout_root_ptr = new_split;
          }
          recompute_layout_bounds(host);
          if (host->focused_pane_ptr) {
            *host->focused_pane_ptr = new_pane;
          }
        }
        *out = lcl_int_new((long)edit_buf->id);
        return LCL_RC_OK;
      }
    } else {
      // Buffer was killed but active flag not cleared - reset state
      annot_edit_clear(host);
      // Fall through to create new editor
    }
  }

  if (argc < 2) {
    lcl_set_error(interp, "annot::edit-range requires start and end");
    return LCL_RC_ERR;
  }

  long start, end;
  if (lcl_value_to_int(argv[0], &start) != LCL_OK || start < 0) {
    lcl_set_error(interp, "annot::edit-range: invalid start");
    return LCL_RC_ERR;
  }
  if (lcl_value_to_int(argv[1], &end) != LCL_OK || end < 0) {
    lcl_set_error(interp, "annot::edit-range: invalid end");
    return LCL_RC_ERR;
  }

  // Query annotations in range
  AnnotQuery query = {0};
  annot_store_in_range(buffer->annots, (size_t)start, (size_t)end, NULL,
                       &query);

  // Store original IDs
  uint32_t *original_ids = NULL;
  if (query.count > 0) {
    original_ids = malloc(query.count * sizeof(uint32_t));
    if (!original_ids) {
      annot_query_free(&query);
      lcl_set_error(interp, "out of memory");
      return LCL_RC_ERR;
    }
    memcpy(original_ids, query.ids, query.count * sizeof(uint32_t));
  }
  size_t original_count = query.count;

  // Generate editor content
  char *content = generate_annot_editor_content(
      buffer, query.ids, query.count, (size_t)start, (size_t)end, true);
  annot_query_free(&query);

  if (!content) {
    free(original_ids);
    lcl_set_error(interp, "failed to generate editor content");
    return LCL_RC_ERR;
  }

  // Create editor buffer
  Buffer *edit_buffer = buffer_manager_create(host->buffer_manager);
  if (!edit_buffer) {
    free(original_ids);
    free(content);
    lcl_set_error(interp, "failed to create buffer");
    return LCL_RC_ERR;
  }

  document_insert(edit_buffer->document, 0, content, strlen(content));
  free(content);

  edit_buffer->file_path = strdup("*annot-edit*");

  // Create pane
  Pane *new_pane = pane_new(edit_buffer, host->char_width, host->line_height);
  if (!new_pane) {
    buffer_manager_close(host->buffer_manager, edit_buffer);
    free(original_ids);
    lcl_set_error(interp, "failed to create pane");
    return LCL_RC_ERR;
  }

  // Apply Lcl syntax highlighting to the annotation editor
  ViewLayer *lcl_layer = tree_sitter_layer_new_lcl(0);
  if (lcl_layer && new_pane->layer_stack) {
    layer_stack_add(new_pane->layer_stack, lcl_layer);
  }

  // Split to show new pane
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }

  // Store state
  host->annot_edit.target_buffer = buffer;
  host->annot_edit.original_ids = original_ids;
  host->annot_edit.original_count = original_count;
  host->annot_edit.doc_rev = annot_store_get_doc_rev(buffer->annots);
  host->annot_edit.query_start = (size_t)start;
  host->annot_edit.query_end = (size_t)end;
  host->annot_edit.is_range = true;
  host->annot_edit.active = true;

  *out = lcl_int_new((long)original_count);
  return LCL_RC_OK;
}

// weft::annot::apply-edit -> apply pending annotation edits
static int c_annot_apply_edit(lcl_interp *interp, int argc, lcl_value **argv,
                              lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (!host->annot_edit.active) {
    lcl_set_error(interp, "no annotation edit in progress");
    return LCL_RC_ERR;
  }

  Buffer *target = host->annot_edit.target_buffer;
  if (!target) {
    annot_edit_clear(host);
    lcl_set_error(interp, "target buffer no longer exists");
    return LCL_RC_ERR;
  }

  // Check staleness
  uint64_t current_rev = annot_store_get_doc_rev(target->annots);
  if (current_rev != host->annot_edit.doc_rev) {
    lcl_set_error(interp,
                  "document modified since edit started - refusing to apply");
    return LCL_RC_ERR;
  }

  // Get the editor buffer content
  Buffer *edit_buffer = host->current_buffer;
  if (!edit_buffer || !edit_buffer->file_path ||
      strcmp(edit_buffer->file_path, "*annot-edit*") != 0) {
    // Try to find it
    edit_buffer = NULL;
    if (host->focused_pane_ptr && *host->focused_pane_ptr) {
      Editor *editor = (*host->focused_pane_ptr)->view;
      if (editor) {
        edit_buffer = editor_get_buffer(editor);
      }
    }
    if (!edit_buffer || !edit_buffer->file_path ||
        strcmp(edit_buffer->file_path, "*annot-edit*") != 0) {
      annot_edit_clear(host);
      lcl_set_error(interp, "annotation editor buffer not found");
      return LCL_RC_ERR;
    }
  }

  Document *doc = edit_buffer->document;
  size_t content_len = document_length(doc);
  char *content = malloc(content_len + 1);
  if (!content) {
    lcl_set_error(interp, "out of memory");
    return LCL_RC_ERR;
  }
  document_get_text(doc, 0, content, content_len);
  content[content_len] = '\0';

  // Parse blocks from editor content
  size_t block_count = 0;
  AnnotEditBlock *blocks =
      parse_annot_blocks(content, content_len, &block_count);
  free(content);

  if (!blocks && block_count > 0) {
    lcl_set_error(interp, "failed to parse annotation blocks");
    return LCL_RC_ERR;
  }

  // Build set of IDs found in the editor
  bool *id_found = NULL;
  if (host->annot_edit.original_count > 0) {
    id_found = calloc(host->annot_edit.original_count, sizeof(bool));
    if (!id_found) {
      for (size_t i = 0; i < block_count; i++) {
        annot_edit_block_free(&blocks[i]);
      }
      free(blocks);
      lcl_set_error(interp, "out of memory");
      return LCL_RC_ERR;
    }
  }

  AnnotStore *store = target->annots;
  size_t added = 0, modified = 0, deleted = 0;

  // Process each block
  for (size_t i = 0; i < block_count; i++) {
    AnnotEditBlock *b = &blocks[i];

    if (b->is_new) {
      // New annotation - add it
      // Use explicit start/end if provided, otherwise use scope range
      size_t start = b->has_start ? b->start : host->annot_edit.query_start;
      size_t end = b->has_end ? b->end : host->annot_edit.query_end;

      const char **keys = (const char **)b->keys;
      const char **values = (const char **)b->values;

      uint32_t new_id = annot_store_add(store, start, end, b->layer, keys,
                                        values, b->meta_count);
      if (new_id != 0) {
        added++;
      }
    } else {
      // Existing annotation - check if it's in our original set
      bool is_original = false;
      for (size_t j = 0; j < host->annot_edit.original_count; j++) {
        if (host->annot_edit.original_ids[j] == b->id) {
          is_original = true;
          id_found[j] = true;
          break;
        }
      }

      if (!is_original) {
        // ID not in original set - skip with warning
        fprintf(stderr,
                "Warning: annotation ID %u not in original set, skipping\n",
                b->id);
        continue;
      }

      // Check if metadata changed
      const AnnotRecord *rec = annot_store_get(store, b->id);
      if (!rec) {
        continue;
      }

      // Get original span (we use original positions, not edited ones)
      size_t orig_start, orig_end;

      annot_store_get_span(store, b->id, &orig_start, &orig_end);

      // Count non-prov metadata in original
      size_t orig_meta_count = 0;
      for (size_t j = 0; j < rec->meta_count; j++) {
        if (strncmp(rec->keys[j], "prov:", 5) != 0) {
          orig_meta_count++;
        }
      }

      // Check if metadata changed
      bool changed = false;
      if (b->meta_count != orig_meta_count) {
        changed = true;
      } else if (strcmp(b->layer, rec->layer ? rec->layer : "") != 0) {
        changed = true;
      } else {
        // Compare each key-value
        for (size_t j = 0; j < b->meta_count && !changed; j++) {
          const char *orig_val = NULL;
          for (size_t k = 0; k < rec->meta_count; k++) {
            if (strncmp(rec->keys[k], "prov:", 5) != 0 &&
                strcmp(rec->keys[k], b->keys[j]) == 0) {
              orig_val = rec->values[k];
              break;
            }
          }
          if (!orig_val || strcmp(orig_val, b->values[j]) != 0) {
            changed = true;
          }
        }
      }

      if (changed) {
        // Delete and recreate with new metadata
        annot_store_remove(store, b->id);

        const char **keys = (const char **)b->keys;
        const char **values = (const char **)b->values;

        annot_store_add(store, orig_start, orig_end, b->layer, keys, values,
                        b->meta_count);
        modified++;
      }
    }
  }

  // Delete annotations that were removed from the editor
  for (size_t i = 0; i < host->annot_edit.original_count; i++) {
    if (!id_found[i]) {
      annot_store_remove(store, host->annot_edit.original_ids[i]);
      deleted++;
    }
  }

  // Cleanup
  free(id_found);
  for (size_t i = 0; i < block_count; i++) {
    annot_edit_block_free(&blocks[i]);
  }
  free(blocks);

  // Clear state
  annot_edit_clear(host);

  // Kill the editor pane (destroys *annot-edit* buffer)
  kill_focused_pane(host);

  // Return summary
  char result[128];
  snprintf(result, sizeof(result), "added %zu, modified %zu, deleted %zu",
           added, modified, deleted);
  *out = lcl_string_new(result);
  return LCL_RC_OK;
}

// weft::annot::cancel-edit -> cancel pending annotation edits
static int c_annot_cancel_edit(lcl_interp *interp, int argc, lcl_value **argv,
                               lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (!host->annot_edit.active) {
    *out = lcl_string_new("no edit in progress");
    return LCL_RC_OK;
  }

  // Clear state
  annot_edit_clear(host);

  // Kill the editor pane (destroys *annot-edit* buffer)
  kill_focused_pane(host);

  *out = lcl_string_new("edit cancelled");
  return LCL_RC_OK;
}

// weft::annot::persist layer -> mark layer as persistent (saved to disk)
static int c_annot_persist(lcl_interp *interp, int argc, lcl_value **argv,
                           lcl_value **out) {
  if (argc < 1) {
    lcl_set_error(interp, "weft::annot::persist requires layer name");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  const char *layer = lcl_value_to_string(argv[0]);
  if (!layer) {
    lcl_set_error(interp, "invalid layer name");
    return LCL_RC_ERR;
  }

  lcl_host_set_layer_persistent(host, layer);
  *out = lcl_int_new(1);
  return LCL_RC_OK;
}

// weft::annot::ephemeral layer -> mark layer as ephemeral (not saved)
static int c_annot_ephemeral(lcl_interp *interp, int argc, lcl_value **argv,
                             lcl_value **out) {
  if (argc < 1) {
    lcl_set_error(interp, "weft::annot::ephemeral requires layer name");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  const char *layer = lcl_value_to_string(argv[0]);
  if (!layer) {
    lcl_set_error(interp, "invalid layer name");
    return LCL_RC_ERR;
  }

  lcl_host_set_layer_ephemeral(host, layer);
  *out = lcl_int_new(1);
  return LCL_RC_OK;
}

// weft::annot::orphans -> list orphaned annotations for current buffer
static int c_annot_orphans(lcl_interp *interp, int argc, lcl_value **argv,
                           lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  Buffer *buffer = host->current_buffer;
  if (host->repl_attached_buffer) {
    buffer = host->repl_attached_buffer;
  }
  if (!buffer) {
    lcl_set_error(interp, "no current buffer");
    return LCL_RC_ERR;
  }

  lcl_value *list = lcl_list_new();
  size_t orphan_count = buffer_orphan_count(buffer);

  for (size_t i = 0; i < orphan_count; i++) {
    const PersistedAnnot *orphan = buffer_get_orphan(buffer, i);
    if (!orphan) {
      continue;
    }

    // Build entry: {index layer approx_start approx_end {key val ...}}
    lcl_value *entry = lcl_list_new();
    lcl_list_push(&entry, lcl_int_new((long)i));
    lcl_list_push(&entry, lcl_string_new(orphan->layer ? orphan->layer : ""));
    lcl_list_push(&entry, lcl_int_new((long)orphan->start.approx_pos));
    lcl_list_push(&entry, lcl_int_new((long)orphan->end.approx_pos));

    // Metadata as nested list
    lcl_value *meta_list = lcl_list_new();
    for (size_t j = 0; j < orphan->meta_count; j++) {
      lcl_list_push(&meta_list, lcl_string_new(orphan->keys[j]));
      lcl_list_push(&meta_list, lcl_string_new(orphan->values[j]));
    }
    lcl_list_push(&entry, meta_list);

    lcl_list_push(&list, entry);
  }

  *out = list;
  return LCL_RC_OK;
}

// weft::annot::resolve-orphan idx start end -> manually re-anchor orphan
static int c_annot_resolve_orphan(lcl_interp *interp, int argc,
                                  lcl_value **argv, lcl_value **out) {
  if (argc < 3) {
    lcl_set_error(interp, "weft::annot::resolve-orphan requires idx start end");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  Buffer *buffer = host->current_buffer;
  if (host->repl_attached_buffer) {
    buffer = host->repl_attached_buffer;
  }
  if (!buffer) {
    lcl_set_error(interp, "no current buffer");
    return LCL_RC_ERR;
  }

  long idx, start, end;
  if (lcl_value_to_int(argv[0], &idx) != LCL_OK || idx < 0) {
    lcl_set_error(interp, "invalid index");
    return LCL_RC_ERR;
  }
  if (lcl_value_to_int(argv[1], &start) != LCL_OK || start < 0) {
    lcl_set_error(interp, "invalid start position");
    return LCL_RC_ERR;
  }
  if (lcl_value_to_int(argv[2], &end) != LCL_OK || end < 0) {
    lcl_set_error(interp, "invalid end position");
    return LCL_RC_ERR;
  }

  uint32_t id =
      buffer_resolve_orphan(buffer, (size_t)idx, (size_t)start, (size_t)end);

  *out = lcl_int_new((long)id);
  return LCL_RC_OK;
}

// weft::annot::discard-orphan idx -> discard orphaned annotation
static int c_annot_discard_orphan(lcl_interp *interp, int argc,
                                  lcl_value **argv, lcl_value **out) {
  if (argc < 1) {
    lcl_set_error(interp, "weft::annot::discard-orphan requires idx");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  Buffer *buffer = host->current_buffer;
  if (host->repl_attached_buffer) {
    buffer = host->repl_attached_buffer;
  }
  if (!buffer) {
    lcl_set_error(interp, "no current buffer");
    return LCL_RC_ERR;
  }

  long idx;
  if (lcl_value_to_int(argv[0], &idx) != LCL_OK || idx < 0) {
    lcl_set_error(interp, "invalid index");
    return LCL_RC_ERR;
  }

  buffer_discard_orphan(buffer, (size_t)idx);

  *out = lcl_int_new(1);
  return LCL_RC_OK;
}

// === Internal: Interaction Bindings ===

// weft::interact::event-type -> returns "left", "middle", "right", or "hover"
static int c_interact_event_type(lcl_interp *interp, int argc, lcl_value **argv,
                                 lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_event) {
    *out = lcl_string_new("");
    return LCL_RC_OK;
  }

  const char *type_str;
  switch (host->current_event->type) {
  case INTERACT_LEFT_CLICK: type_str = "left"; break;
  case INTERACT_MIDDLE_CLICK: type_str = "middle"; break;
  case INTERACT_RIGHT_CLICK: type_str = "right"; break;
  case INTERACT_HOVER: type_str = "hover"; break;
  default: type_str = "unknown"; break;
  }

  *out = lcl_string_new(type_str);
  return LCL_RC_OK;
}

// weft::interact::position -> returns document byte position
static int c_interact_position(lcl_interp *interp, int argc, lcl_value **argv,
                               lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_event) {
    *out = lcl_int_new(0);
    return LCL_RC_OK;
  }

  *out = lcl_int_new((long)host->current_event->doc_pos);
  return LCL_RC_OK;
}

// weft::interact::selection -> returns {start end text} or empty list
static int c_interact_selection(lcl_interp *interp, int argc, lcl_value **argv,
                                lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_event || !host->current_event->has_selection) {
    *out = lcl_list_new();
    return LCL_RC_OK;
  }

  lcl_value *list = lcl_list_new();
  lcl_list_push(&list, lcl_int_new((long)host->current_event->sel_start));
  lcl_list_push(&list, lcl_int_new((long)host->current_event->sel_end));
  lcl_list_push(&list, lcl_string_new(host->current_event->selected_text
                                          ? host->current_event->selected_text
                                          : ""));
  *out = list;
  return LCL_RC_OK;
}

// weft::interact::word-at-pos -> returns word under cursor
static int c_interact_word_at_pos(lcl_interp *interp, int argc,
                                  lcl_value **argv, lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_event || !host->current_event->word_at_pos) {
    *out = lcl_string_new("");
    return LCL_RC_OK;
  }

  *out = lcl_string_new(host->current_event->word_at_pos);
  return LCL_RC_OK;
}

// weft::interact::modifiers -> returns list of held modifiers
static int c_interact_modifiers(lcl_interp *interp, int argc, lcl_value **argv,
                                lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  lcl_value *list = lcl_list_new();

  if (!host || !host->current_event) {
    *out = list;
    return LCL_RC_OK;
  }

  SDL_Keymod mod = host->current_event->modifiers;
  if (mod & SDL_KMOD_CTRL) {
    lcl_list_push(&list, lcl_string_new("ctrl"));
  }
  if (mod & SDL_KMOD_SHIFT) {
    lcl_list_push(&list, lcl_string_new("shift"));
  }
  if (mod & SDL_KMOD_ALT) {
    lcl_list_push(&list, lcl_string_new("alt"));
  }

  *out = list;
  return LCL_RC_OK;
}

// weft::interact::handled -> mark event as consumed
static int c_interact_handled(lcl_interp *interp, int argc, lcl_value **argv,
                              lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (host && host->current_event) {
    host->current_event->handled = true;
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// === Internal: Editor Bindings ===

// weft::editor::cursor -> get cursor position
static int c_editor_cursor(lcl_interp *interp, int argc, lcl_value **argv,
                           lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_editor) {
    *out = lcl_int_new(0);
    return LCL_RC_OK;
  }

  *out = lcl_int_new((long)editor_get_cursor_pos(host->current_editor));
  return LCL_RC_OK;
}

// weft::editor::cursor-set pos -> set cursor position
static int c_editor_cursor_set(lcl_interp *interp, int argc, lcl_value **argv,
                               lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_editor) {
    lcl_set_error(interp, "no editor context");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "editor::cursor-set requires position");
    return LCL_RC_ERR;
  }

  long pos;
  if (lcl_value_to_int(argv[0], &pos) != LCL_OK || pos < 0) {
    lcl_set_error(interp, "editor::cursor-set: invalid position");
    return LCL_RC_ERR;
  }

  Editor *e = host->current_editor;
  Buffer *buf = editor_get_buffer(e);
  size_t doc_len = document_length(buf->document);

  // Clamp to document bounds
  if ((size_t)pos > doc_len) {
    pos = (long)doc_len;
  }

  // Access editor internals through public-ish API
  // Note: This requires cursor_pos to be accessible
  e->cursor_pos = (size_t)pos;
  editor_clear_selection(e);
  editor_scroll_to_cursor(e);

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::editor::insert pos text -> insert text at position
static int c_editor_insert(lcl_interp *interp, int argc, lcl_value **argv,
                           lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_editor) {
    lcl_set_error(interp, "no editor context");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp, "editor::insert requires position and text");
    return LCL_RC_ERR;
  }

  long pos;
  if (lcl_value_to_int(argv[0], &pos) != LCL_OK || pos < 0) {
    lcl_set_error(interp, "editor::insert: invalid position");
    return LCL_RC_ERR;
  }

  const char *text = lcl_value_to_string(argv[1]);
  if (!text) {
    lcl_set_error(interp, "editor::insert: text must be a string");
    return LCL_RC_ERR;
  }

  Buffer *buf = editor_get_buffer(host->current_editor);
  size_t len = strlen(text);

  if (document_insert(buf->document, (size_t)pos, text, len)) {
    annot_store_on_insert(buf->annots, (size_t)pos, len);
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::editor::delete start end -> delete range
static int c_editor_delete(lcl_interp *interp, int argc, lcl_value **argv,
                           lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_editor) {
    lcl_set_error(interp, "no editor context");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp, "editor::delete requires start and end");
    return LCL_RC_ERR;
  }

  long start, end;
  if (lcl_value_to_int(argv[0], &start) != LCL_OK || start < 0) {
    lcl_set_error(interp, "editor::delete: invalid start");
    return LCL_RC_ERR;
  }
  if (lcl_value_to_int(argv[1], &end) != LCL_OK || end < 0) {
    lcl_set_error(interp, "editor::delete: invalid end");
    return LCL_RC_ERR;
  }

  if (end < start) {
    long tmp = start;
    start = end;
    end = tmp;
  }

  Buffer *buf = editor_get_buffer(host->current_editor);
  size_t len = (size_t)(end - start);

  if (document_delete(buf->document, (size_t)start, len)) {
    annot_store_on_delete(buf->annots, (size_t)start, len);
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// === Internal: Shell Bindings ===

// Helper: Execute command and capture output
static int shell_exec_internal(const char *cmd, char **out_stdout,
                               char **out_stderr, int *status) {
  int stdout_pipe[2], stderr_pipe[2];

  if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    return -1;
  }

  if (pid == 0) {
    // Child process
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    execl("/bin/sh", "sh", "-c", cmd, NULL);
    _exit(127);
  }

  // Parent process
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  // Read stdout
  size_t stdout_cap = 4096;
  size_t stdout_len = 0;
  char *stdout_buf = malloc(stdout_cap);
  if (stdout_buf) {
    ssize_t n;
    while ((n = read(stdout_pipe[0], stdout_buf + stdout_len,
                     stdout_cap - stdout_len - 1)) > 0) {
      stdout_len += (size_t)n;
      if (stdout_len >= stdout_cap - 1) {
        stdout_cap *= 2;
        char *new_buf = realloc(stdout_buf, stdout_cap);
        if (!new_buf) {
          break;
        }
        stdout_buf = new_buf;
      }
    }
    stdout_buf[stdout_len] = '\0';
  }
  close(stdout_pipe[0]);

  // Read stderr
  size_t stderr_cap = 4096;
  size_t stderr_len = 0;
  char *stderr_buf = malloc(stderr_cap);
  if (stderr_buf) {
    ssize_t n;
    while ((n = read(stderr_pipe[0], stderr_buf + stderr_len,
                     stderr_cap - stderr_len - 1)) > 0) {
      stderr_len += (size_t)n;
      if (stderr_len >= stderr_cap - 1) {
        stderr_cap *= 2;
        char *new_buf = realloc(stderr_buf, stderr_cap);
        if (!new_buf) {
          break;
        }
        stderr_buf = new_buf;
      }
    }
    stderr_buf[stderr_len] = '\0';
  }
  close(stderr_pipe[0]);

  // Wait for child
  int wstatus;
  waitpid(pid, &wstatus, 0);
  *status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;

  *out_stdout = stdout_buf;
  *out_stderr = stderr_buf;

  return 0;
}

// weft::shell::exec cmd -> returns {status stdout stderr}
static int c_shell_exec(lcl_interp *interp, int argc, lcl_value **argv,
                        lcl_value **out) {
  if (argc < 1) {
    lcl_set_error(interp, "shell::exec requires command");
    return LCL_RC_ERR;
  }

  const char *cmd = lcl_value_to_string(argv[0]);
  if (!cmd) {
    lcl_set_error(interp, "shell::exec: command must be a string");
    return LCL_RC_ERR;
  }

  char *stdout_str = NULL;
  char *stderr_str = NULL;
  int status = 0;

  if (shell_exec_internal(cmd, &stdout_str, &stderr_str, &status) < 0) {
    lcl_set_error(interp, "shell::exec: failed to execute command");
    return LCL_RC_ERR;
  }

  // Return as list: {status stdout stderr}
  lcl_value *result = lcl_list_new();
  lcl_list_push(&result, lcl_string_new("status"));
  lcl_list_push(&result, lcl_int_new(status));
  lcl_list_push(&result, lcl_string_new("stdout"));
  lcl_list_push(&result, lcl_string_new(stdout_str ? stdout_str : ""));
  lcl_list_push(&result, lcl_string_new("stderr"));
  lcl_list_push(&result, lcl_string_new(stderr_str ? stderr_str : ""));

  free(stdout_str);
  free(stderr_str);

  *out = result;
  return LCL_RC_OK;
}

// weft::shell::pipe-selection cmd -> pipe selection through command, replace
// with output
static int c_shell_pipe_selection(lcl_interp *interp, int argc,
                                  lcl_value **argv, lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_editor) {
    lcl_set_error(interp, "no editor context");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "shell::pipe-selection requires command");
    return LCL_RC_ERR;
  }

  const char *cmd = lcl_value_to_string(argv[0]);
  if (!cmd) {
    lcl_set_error(interp, "shell::pipe-selection: command must be a string");
    return LCL_RC_ERR;
  }

  // Get selection or word at cursor
  Editor *e = host->current_editor;
  Buffer *buf = editor_get_buffer(e);
  size_t start, end;

  if (editor_has_selection(e)) {
    editor_get_selection(e, &start, &end);
  } else if (host->current_event && host->current_event->word_at_pos) {
    // Use word at position
    // TODO: get actual word bounds
    start = host->current_event->doc_pos;
    end = host->current_event->doc_pos;
  } else {
    *out = lcl_string_new("");
    return LCL_RC_OK;
  }

  size_t sel_len = end - start;
  if (sel_len == 0) {
    *out = lcl_string_new("");
    return LCL_RC_OK;
  }

  // Get selection text
  char *input = malloc(sel_len + 1);
  if (!input) {
    lcl_set_error(interp, "out of memory");
    return LCL_RC_ERR;
  }
  document_get_text(buf->document, start, input, sel_len);
  input[sel_len] = '\0';

  // Create pipe command with echo
  size_t cmd_len = strlen(cmd) + sel_len + 32;
  char *full_cmd = malloc(cmd_len);
  if (!full_cmd) {
    free(input);
    lcl_set_error(interp, "out of memory");
    return LCL_RC_ERR;
  }

  // Escape single quotes in input for shell
  // For simplicity, use printf approach
  snprintf(full_cmd, cmd_len, "printf '%%s' '%s' | %s", input, cmd);

  char *stdout_str = NULL;
  char *stderr_str = NULL;
  int status = 0;

  if (shell_exec_internal(full_cmd, &stdout_str, &stderr_str, &status) < 0) {
    free(input);
    free(full_cmd);
    lcl_set_error(interp, "shell::pipe-selection: failed to execute command");
    return LCL_RC_ERR;
  }

  free(input);
  free(full_cmd);

  if (status == 0 && stdout_str) {
    // Replace selection with output
    size_t output_len = strlen(stdout_str);

    // Remove trailing newline if present
    if (output_len > 0 && stdout_str[output_len - 1] == '\n') {
      stdout_str[output_len - 1] = '\0';
      output_len--;
    }

    // Delete selection
    if (document_delete(buf->document, start, sel_len)) {
      annot_store_on_delete(buf->annots, start, sel_len);
    }

    // Insert output
    if (output_len > 0 &&
        document_insert(buf->document, start, stdout_str, output_len)) {
      annot_store_on_insert(buf->annots, start, output_len);
    }

    // Update cursor
    e->cursor_pos = start + output_len;
    editor_clear_selection(e);
  }

  free(stdout_str);
  free(stderr_str);

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// === Internal: Buffer Bindings ===

// weft::buffer::id -> get the current buffer's ID
static int c_buffer_id(lcl_interp *interp, int argc, lcl_value **argv,
                       lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  Buffer *buf = get_effective_doc_buffer(host);
  if (!buf) {
    lcl_set_error(interp, "no current buffer");
    return LCL_RC_ERR;
  }

  *out = lcl_int_new((int64_t)buf->id);
  return LCL_RC_OK;
}

// weft::buffer::by-id id -> get buffer info by ID, returns {id path modified}
static int c_buffer_by_id(lcl_interp *interp, int argc, lcl_value **argv,
                          lcl_value **out) {
  if (argc < 1) {
    lcl_set_error(interp, "buffer::by-id requires id");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  long id;
  if (lcl_value_to_int(argv[0], &id) != LCL_OK || id <= 0) {
    lcl_set_error(interp, "buffer::by-id: id must be a positive integer");
    return LCL_RC_ERR;
  }

  Buffer *buf = buffer_manager_find_by_id(host->buffer_manager, (uint32_t)id);
  if (!buf) {
    *out = lcl_list_new(); // Empty list if not found
    return LCL_RC_OK;
  }

  // Return {id path modified}
  lcl_value *result = lcl_list_new();
  lcl_list_push(&result, lcl_int_new((long)buf->id));
  lcl_list_push(&result, lcl_string_new(buf->file_path ? buf->file_path : ""));
  lcl_list_push(&result, lcl_int_new(buf->modified ? 1 : 0));
  *out = result;
  return LCL_RC_OK;
}

// weft::buffer::list -> list all open buffers as {{id path modified} ...}
static int c_buffer_list(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  lcl_value *result = lcl_list_new();
  BufferManager *bm = host->buffer_manager;

  for (size_t i = 0; i < bm->count; i++) {
    Buffer *buf = bm->buffers[i];
    lcl_value *entry = lcl_list_new();
    lcl_list_push(&entry, lcl_int_new((long)buf->id));
    lcl_list_push(&entry, lcl_string_new(buf->file_path ? buf->file_path : ""));
    lcl_list_push(&entry, lcl_int_new(buf->modified ? 1 : 0));
    lcl_list_push(&result, entry);
  }

  *out = result;
  return LCL_RC_OK;
}

// Helper struct for tracking action positions in buffer menu
typedef struct {
  size_t start;
  size_t end;
  char handler[256];
} BufferMenuAction;

// weft::buffer::menu -> insert buffer list with clickable actions at cursor
static int c_buffer_menu(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager || !host->current_buffer ||
      !host->current_editor) {
    lcl_set_error(interp, "no buffer/editor context");
    return LCL_RC_ERR;
  }

  BufferManager *bm = host->buffer_manager;
  if (bm->count == 0) {
    *out = lcl_string_new("No buffers open");
    return LCL_RC_OK;
  }

  // Build menu text and track action positions
  // Max actions: 4 per buffer (Show, Kill, Attach/Detach, Save) * 64 buffers
  BufferMenuAction actions[256];
  size_t action_count = 0;

  char menu[8192];
  size_t pos = 0;

  pos += snprintf(menu + pos, sizeof(menu) - pos, ";; Buffers:\n");

  for (size_t i = 0; i < bm->count && pos < sizeof(menu) - 512; i++) {
    Buffer *buf = bm->buffers[i];
    bool is_repl = buf->file_path && strcmp(buf->file_path, "*lcl*") == 0;
    bool is_attached = (host->repl_attached_buffer == buf);

    // Get display name
    const char *filepath = buf->file_path;
    const char *display_name = filepath ? filepath : "[untitled]";
    if (filepath) {
      const char *last_slash = strrchr(filepath, '/');
      if (last_slash) {
        display_name = last_slash + 1;
      }
    }

    // [Show] action
    if (action_count < 256) {
      size_t action_start = pos;
      pos += snprintf(menu + pos, sizeof(menu) - pos, "[Show]");
      actions[action_count].start = action_start;
      actions[action_count].end = pos;
      snprintf(actions[action_count].handler, sizeof(actions[0].handler),
               "weft::buffer::show %u\nweft::interact::handled", buf->id);
      action_count++;
    }

    pos += snprintf(menu + pos, sizeof(menu) - pos, " ");

    // [Kill] action
    if (action_count < 256) {
      size_t action_start = pos;
      pos += snprintf(menu + pos, sizeof(menu) - pos, "[Kill]");
      actions[action_count].start = action_start;
      actions[action_count].end = pos;
      snprintf(actions[action_count].handler, sizeof(actions[0].handler),
               "weft::buffer::kill %u\nweft::interact::handled", buf->id);
      action_count++;
    }

    // [Attach]/[Detach] action (not for *lcl* buffer)
    if (!is_repl) {
      pos += snprintf(menu + pos, sizeof(menu) - pos, " ");
      if (is_attached) {
        // Show [Detach] for the currently attached buffer
        if (action_count < 256) {
          size_t action_start = pos;
          pos += snprintf(menu + pos, sizeof(menu) - pos, "[Detach]");
          actions[action_count].start = action_start;
          actions[action_count].end = pos;
          snprintf(actions[action_count].handler, sizeof(actions[0].handler),
                   "weft::repl::detach\nweft::interact::handled");
          action_count++;
        }
      } else {
        // Show [Attach] for other non-REPL buffers
        if (action_count < 256) {
          size_t action_start = pos;
          pos += snprintf(menu + pos, sizeof(menu) - pos, "[Attach]");
          actions[action_count].start = action_start;
          actions[action_count].end = pos;
          snprintf(actions[action_count].handler, sizeof(actions[0].handler),
                   "weft::repl::attach %u\nweft::interact::handled", buf->id);
          action_count++;
        }
      }
    }

    // [Save] action (only for buffers with a file path)
    if (filepath && filepath[0] != '*') {
      pos += snprintf(menu + pos, sizeof(menu) - pos, " ");
      if (action_count < 256) {
        size_t action_start = pos;
        pos += snprintf(menu + pos, sizeof(menu) - pos, "[Save]");
        actions[action_count].start = action_start;
        actions[action_count].end = pos;
        snprintf(actions[action_count].handler, sizeof(actions[0].handler),
                 "weft::buffer::save %u\nweft::interact::handled", buf->id);
        action_count++;
      }
    }

    // Buffer ID and name
    pos += snprintf(menu + pos, sizeof(menu) - pos, "   %u  %s%s\n", buf->id,
                    display_name, buf->modified ? " (modified)" : "");
  }

  // Insert text at cursor
  size_t cursor = editor_get_cursor_pos(host->current_editor);
  document_insert(host->current_buffer->document, cursor, menu, strlen(menu));

  // Add annotations for each action
  for (size_t i = 0; i < action_count; i++) {
    const char *keys[] = {"action:on_middle_click", "style:fg"};
    const char *vals[] = {actions[i].handler, "#88AAFF"};
    annot_store_add(host->current_buffer->annots, cursor + actions[i].start,
                    cursor + actions[i].end, "action", keys, vals, 2);
  }

  // Move cursor to end of inserted text
  editor_set_cursor_pos(host->current_editor, cursor + strlen(menu));

  *out = lcl_int_new((long)bm->count);
  return LCL_RC_OK;
}

// weft::buffer::close id -> close buffer by ID (closes pane containing it)
static int c_buffer_close(lcl_interp *interp, int argc, lcl_value **argv,
                          lcl_value **out) {
  if (argc < 1) {
    lcl_set_error(interp, "buffer::close requires id");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  long id;
  if (lcl_value_to_int(argv[0], &id) != LCL_OK || id <= 0) {
    lcl_set_error(interp, "buffer::close: id must be a positive integer");
    return LCL_RC_ERR;
  }

  Buffer *buf = buffer_manager_find_by_id(host->buffer_manager, (uint32_t)id);
  if (!buf) {
    lcl_set_error(interp, "buffer::close: no buffer with that id");
    return LCL_RC_ERR;
  }

  // Find pane containing this buffer and close it
  LayoutNode *root = *host->layout_root_ptr;
  Pane *pane = layout_find_pane_with_buffer(root, buf);
  if (!pane) {
    // Buffer exists but isn't displayed - nothing to close
    // Buffer survives for re-summoning via weft::buffer::show
    *out = lcl_int_new(0);
    return LCL_RC_OK;
  }

  // Find next pane to focus
  Pane *next = layout_next_pane(root, pane);
  if (next == pane) {
    next = NULL;
  }

  // Close the pane
  LayoutNode *new_root = layout_close_pane(root, pane);

  if (new_root) {
    *host->layout_root_ptr = new_root;
    *host->focused_pane_ptr = next ? next : layout_first_pane(new_root);
    recompute_layout_bounds(host);
    *out = lcl_int_new(1);
  } else {
    // Last pane - don't close
    *out = lcl_int_new(0);
  }

  return LCL_RC_OK;
}

// weft::buffer::save ?id? -> save buffer (by ID, or current if no arg)
static int c_buffer_save(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  Buffer *buf = NULL;

  if (argc >= 1) {
    // Save by ID
    long id;
    if (lcl_value_to_int(argv[0], &id) != LCL_OK || id <= 0) {
      lcl_set_error(interp, "buffer::save: id must be a positive integer");
      return LCL_RC_ERR;
    }
    buf = buffer_manager_find_by_id(host->buffer_manager, (uint32_t)id);
    if (!buf) {
      lcl_set_error(interp, "buffer::save: no buffer with that id");
      return LCL_RC_ERR;
    }
  } else {
    // Save current buffer
    buf = get_effective_doc_buffer(host);
    if (!buf) {
      lcl_set_error(interp, "no current buffer");
      return LCL_RC_ERR;
    }
  }

  if (!buf->file_path) {
    lcl_set_error(interp, "buffer::save: buffer has no file path");
    return LCL_RC_ERR;
  }

  bool success = buffer_save(buf);

  // Save annotations after saving the file
  if (success && host->data_dir && host->doc_registry) {
    size_t layer_count = 0;
    const char *const *layers =
        lcl_host_get_persistent_layers(host, &layer_count);
    buffer_save_annotations(buf, host->doc_registry, host->data_dir, layers,
                            layer_count);
    // Save the registry to persist any new doc_id mappings
    doc_registry_save(host->doc_registry);
  }

  *out = lcl_int_new(success ? 1 : 0);
  return LCL_RC_OK;
}

// weft::buffer::show id -> show buffer in pane (create pane if needed, focus if
// exists)
static int c_buffer_show(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  if (argc < 1) {
    lcl_set_error(interp, "buffer::show requires id");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  long id;
  if (lcl_value_to_int(argv[0], &id) != LCL_OK || id <= 0) {
    lcl_set_error(interp, "buffer::show: id must be a positive integer");
    return LCL_RC_ERR;
  }

  Buffer *buf = buffer_manager_find_by_id(host->buffer_manager, (uint32_t)id);
  if (!buf) {
    lcl_set_error(interp, "buffer::show: no buffer with that id");
    return LCL_RC_ERR;
  }

  // Check if buffer already has a pane
  Pane *existing_pane = find_pane_for_buffer(host, buf);
  if (existing_pane) {
    // Just focus it
    if (host->focused_pane_ptr) {
      *host->focused_pane_ptr = existing_pane;
    }
    *out = lcl_int_new((long)buf->id);
    return LCL_RC_OK;
  }

  // Create new pane for the buffer
  Pane *new_pane = pane_new(buf, host->char_width, host->line_height);
  if (!new_pane) {
    lcl_set_error(interp, "buffer::show: failed to create pane");
    return LCL_RC_ERR;
  }

  // Apply viewlayer configuration
  lcl_host_apply_viewlayers(host, new_pane);

  // Split current layout horizontally to show new pane
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  // Recompute layout bounds
  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }

  *out = lcl_int_new((long)buf->id);
  return LCL_RC_OK;
}

// weft::buffer::kill id -> destroy buffer completely (close pane and free
// buffer)
static int c_buffer_kill(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  if (argc < 1) {
    lcl_set_error(interp, "buffer::kill requires id");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  long id;
  if (lcl_value_to_int(argv[0], &id) != LCL_OK || id <= 0) {
    lcl_set_error(interp, "buffer::kill: id must be a positive integer");
    return LCL_RC_ERR;
  }

  Buffer *buf = buffer_manager_find_by_id(host->buffer_manager, (uint32_t)id);
  if (!buf) {
    lcl_set_error(interp, "buffer::kill: no buffer with that id");
    return LCL_RC_ERR;
  }

  // Clear associated state before closing
  // If this buffer is the annotation edit target, clear that state
  if (host->annot_edit.target_buffer == buf) {
    annot_edit_clear(host);
  }
  // If this is the *annot-edit* buffer itself, clear annotation edit state
  if (buf->file_path && strcmp(buf->file_path, "*annot-edit*") == 0) {
    annot_edit_clear(host);
  }
  // If this buffer is the patch target, clear pending patch
  if (host->patch_target_buffer == buf) {
    if (host->pending_patch) {
      patch_free(host->pending_patch);
      host->pending_patch = NULL;
    }
    host->patch_target_buffer = NULL;
  }
  // If this is the *diff* buffer, clear pending patch
  if (buf->file_path && strcmp(buf->file_path, "*diff*") == 0) {
    if (host->pending_patch) {
      patch_free(host->pending_patch);
      host->pending_patch = NULL;
    }
    host->patch_target_buffer = NULL;
  }
  // If this buffer is the REPL attached buffer, detach
  if (host->repl_attached_buffer == buf) {
    host->repl_attached_buffer = NULL;
  }
  // If this buffer is the current buffer, clear it
  if (host->current_buffer == buf) {
    host->current_buffer = NULL;
    host->current_editor = NULL;
  }

  // Find pane containing this buffer and close it (if any)
  LayoutNode *root = *host->layout_root_ptr;
  Pane *pane = layout_find_pane_with_buffer(root, buf);

  if (pane) {
    // Find next pane to focus
    Pane *next = layout_next_pane(root, pane);
    if (next == pane) {
      next = NULL;
    }

    // Close the pane
    LayoutNode *new_root = layout_close_pane(root, pane);

    if (new_root) {
      *host->layout_root_ptr = new_root;
      *host->focused_pane_ptr = next ? next : layout_first_pane(new_root);
      recompute_layout_bounds(host);
    } else {
      // Last pane - don't destroy the buffer
      lcl_set_error(interp, "buffer::kill: cannot kill last buffer");
      return LCL_RC_ERR;
    }
  }

  // Destroy the buffer
  buffer_manager_close(host->buffer_manager, buf);

  *out = lcl_int_new(1);
  return LCL_RC_OK;
}

// weft::buffer::new -> opens a new buffer
static int c_buffer_new(lcl_interp *interp, int argc, lcl_value **argv,
                        lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);

  if (!host || !host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  Buffer *new_buffer = buffer_manager_create(host->buffer_manager);

  if (!new_buffer) {
    lcl_set_error(interp, "failed to create new buffer");
    return LCL_RC_ERR;
  }

  // Create a new pane for this buffer using renderer metrics
  Pane *new_pane = pane_new(new_buffer, host->char_width, host->line_height);

  if (!new_pane) {
    buffer_manager_close(host->buffer_manager, new_buffer);
    lcl_set_error(interp, "buffer::new: failed to create pane");
    return LCL_RC_ERR;
  }

  // Apply viewlayer configuration based on file type
  lcl_host_apply_viewlayers(host, new_pane);

  // Apply indent settings based on file type
  lcl_host_apply_indent(host, pane_get_editor(new_pane));

  // Apply wrap settings based on file type
  lcl_host_apply_wrap(host, pane_get_editor(new_pane));

  // Split current pane horizontally to show new pane
  // Find the current focused pane's layout node
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  // Recompute layout bounds
  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }

  *out = lcl_list_new();

  return LCL_RC_OK;
}

// weft::buffer::open path -> open file in new pane
static int c_buffer_open(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "buffer::open requires path");
    return LCL_RC_ERR;
  }

  const char *path = lcl_value_to_string(argv[0]);
  if (!path) {
    lcl_set_error(interp, "buffer::open: path must be a string");
    return LCL_RC_ERR;
  }

  // Open file in buffer manager
  Buffer *new_buffer = buffer_manager_open_file(host->buffer_manager, path);
  if (!new_buffer) {
    lcl_set_error(interp, "buffer::open: failed to open file");
    return LCL_RC_ERR;
  }

  // Load persisted annotations for this file
  if (host->data_dir && host->doc_registry) {
    size_t loaded =
        buffer_load_annotations(new_buffer, host->doc_registry, host->data_dir);
    if (loaded > 0) {
      // Annotations were loaded
      (void)loaded;
    }
  }

  // Apply file type associations (syntax highlighting, etc.)
  lcl_host_apply_filetypes(host, new_buffer);

  // Create a new pane for this buffer using renderer metrics
  Pane *new_pane = pane_new(new_buffer, host->char_width, host->line_height);

  if (!new_pane) {
    buffer_manager_close(host->buffer_manager, new_buffer);
    lcl_set_error(interp, "buffer::open: failed to create pane");
    return LCL_RC_ERR;
  }

  // Apply viewlayer configuration based on file type
  lcl_host_apply_viewlayers(host, new_pane);

  // Apply indent settings based on file type
  lcl_host_apply_indent(host, pane_get_editor(new_pane));

  // Apply wrap settings based on file type
  lcl_host_apply_wrap(host, pane_get_editor(new_pane));

  // Split current pane horizontally to show new pane
  // Find the current focused pane's layout node
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  // Recompute layout bounds
  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::buffer::open-output name content -> create scratch buffer with content
static int c_buffer_open_output(lcl_interp *interp, int argc, lcl_value **argv,
                                lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp, "buffer::open-output requires name and content");
    return LCL_RC_ERR;
  }

  const char *name = lcl_value_to_string(argv[0]);
  const char *content = lcl_value_to_string(argv[1]);
  if (!name || !content) {
    lcl_set_error(interp,
                  "buffer::open-output: name and content must be strings");
    return LCL_RC_ERR;
  }

  // Create new buffer
  Buffer *new_buffer = buffer_manager_create(host->buffer_manager);
  if (!new_buffer) {
    lcl_set_error(interp, "buffer::open-output: failed to create buffer");
    return LCL_RC_ERR;
  }

  // Set content
  size_t content_len = strlen(content);
  if (content_len > 0) {
    document_insert(new_buffer->document, 0, content, content_len);
  }

  // Set a pseudo file path for display
  char *display_name = malloc(strlen(name) + 1);
  if (display_name) {
    strcpy(display_name, name);
    new_buffer->file_path = display_name;
  }

  // Scan for file:line patterns (plumber)
  plumb_scan_buffer(new_buffer);

  // Create new pane using renderer metrics
  Pane *new_pane = pane_new(new_buffer, host->char_width, host->line_height);
  if (!new_pane) {
    buffer_manager_close(host->buffer_manager, new_buffer);
    lcl_set_error(interp, "buffer::open-output: failed to create pane");
    return LCL_RC_ERR;
  }

  // Apply viewlayer configuration
  lcl_host_apply_viewlayers(host, new_pane);

  // Split to show new pane
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  // Recompute layout bounds
  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::repl::open -> open Lcl REPL buffer
static int c_repl_open(lcl_interp *interp, int argc, lcl_value **argv,
                       lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  // Create new buffer
  Buffer *new_buffer = buffer_manager_create(host->buffer_manager);
  if (!new_buffer) {
    lcl_set_error(interp, "repl::open: failed to create buffer");
    return LCL_RC_ERR;
  }

  // Set initial content with welcome message
  const char *welcome =
      ";; Lcl REPL - Weft Scripting Console\n"
      ";; Type Lcl expressions and press Ctrl+Enter to evaluate\n"
      ";; Results appear below each expression\n"
      ";;\n"
      ";; Examples:\n"
      ";;   [+ 1 2]              ;; => 3\n"
      ";;   [weft::doc::length]  ;; => document length\n"
      ";;   [weft::version]      ;; => version string\n"
      ";;\n\n";

  document_insert(new_buffer->document, 0, welcome, strlen(welcome));

  // Set special name to identify as REPL buffer
  new_buffer->file_path = strdup("*lcl*");

  // Apply tree-sitter highlighting for Lcl
  Buffer *prev_buffer = host->current_buffer;
  host->current_buffer = new_buffer;

  // Try to find highlights.scm
  const char *highlights_path = NULL;
  const char *paths[] = {
      "/home/brennan/dev/weft/submodules/tree-sitter-lcl/queries/highlights.scm",
      "../submodules/tree-sitter-lcl/queries/highlights.scm",
      "../../submodules/tree-sitter-lcl/queries/highlights.scm", NULL};
  for (const char **p = paths; *p; p++) {
    FILE *f = fopen(*p, "r");
    if (f) {
      fclose(f);
      highlights_path = *p;
      break;
    }
  }

  if (highlights_path) {
    TSHighlighter *hl = ts_hl_get_lcl(highlights_path);
    if (hl) {
      ts_hl_apply_to_buffer(hl, new_buffer, highlights_path);
    }
  }

  host->current_buffer = prev_buffer;

  // Create new pane using renderer metrics
  Pane *new_pane = pane_new(new_buffer, host->char_width, host->line_height);
  if (!new_pane) {
    buffer_manager_close(host->buffer_manager, new_buffer);
    lcl_set_error(interp, "repl::open: failed to create pane");
    return LCL_RC_ERR;
  }

  // Apply viewlayer configuration (REPL uses *lcl* which triggers .lcl config)
  lcl_host_apply_viewlayers(host, new_pane);

  // Split to show new pane
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  // Recompute layout bounds
  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// Helper: Check if a buffer is the REPL buffer
static bool is_repl_buffer(const Buffer *buf) {
  return buf && buf->file_path && strcmp(buf->file_path, "*lcl*") == 0;
}

// Helper: Get the effective buffer for doc commands
// If in REPL and attached to another buffer, use that buffer
static Buffer *get_effective_doc_buffer(LclHost *host) {
  if (!host || !host->current_buffer) {
    return NULL;
  }

  // If current buffer is REPL and we have an attached buffer, use it
  if (is_repl_buffer(host->current_buffer) && host->repl_attached_buffer) {
    return host->repl_attached_buffer;
  }

  return host->current_buffer;
}

// weft::repl::attach -> attach REPL to the previously focused buffer
static int c_repl_attach(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  Buffer *target = NULL;

  // If an argument is provided, treat it as a buffer ID
  if (argc >= 1) {
    long id;
    if (lcl_value_to_int(argv[0], &id) != LCL_OK || id <= 0) {
      lcl_set_error(interp, "repl::attach: id must be a positive integer");
      return LCL_RC_ERR;
    }
    target = buffer_manager_find_by_id(host->buffer_manager, (uint32_t)id);
    if (!target) {
      lcl_set_error(interp, "repl::attach: no buffer with that id");
      return LCL_RC_ERR;
    }
    // Don't allow attaching to the REPL itself
    if (is_repl_buffer(target)) {
      lcl_set_error(interp, "repl::attach: cannot attach to the REPL itself");
      return LCL_RC_ERR;
    }
  } else {
    // No argument - use current buffer (original behavior)
    if (!host->current_buffer) {
      lcl_set_error(interp, "no current buffer");
      return LCL_RC_ERR;
    }

    // Don't attach to the REPL itself
    if (is_repl_buffer(host->current_buffer)) {
      // If we're in the REPL and there's no attachment yet, this is a no-op
      // User needs to call this from a non-REPL buffer or provide an ID
      if (!host->repl_attached_buffer) {
        lcl_set_error(interp,
                      "repl::attach requires buffer id when called from REPL");
        return LCL_RC_ERR;
      }
      // Already attached - return info
      *out = lcl_string_new(host->repl_attached_buffer->file_path
                                ? host->repl_attached_buffer->file_path
                                : "(untitled)");
      return LCL_RC_OK;
    }

    target = host->current_buffer;
  }

  // Attach to target buffer
  host->repl_attached_buffer = target;

  *out = lcl_string_new(target->file_path ? target->file_path : "(untitled)");
  return LCL_RC_OK;
}

// weft::repl::detach -> detach REPL from attached buffer
static int c_repl_detach(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (!host->repl_attached_buffer) {
    *out = lcl_string_new("not attached");
    return LCL_RC_OK;
  }

  const char *name = host->repl_attached_buffer->file_path
                         ? host->repl_attached_buffer->file_path
                         : "(untitled)";
  char msg[256];
  snprintf(msg, sizeof(msg), "detached from %s", name);

  host->repl_attached_buffer = NULL;

  *out = lcl_string_new(msg);
  return LCL_RC_OK;
}

// weft::repl::target -> get info about attached buffer
static int c_repl_target(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (!host->repl_attached_buffer) {
    *out = lcl_list_new(); // Empty list = not attached
    return LCL_RC_OK;
  }

  Buffer *buf = host->repl_attached_buffer;
  Document *doc = buf->document;

  // Return a list with buffer info
  lcl_value *result = lcl_list_new();
  lcl_list_push(&result, lcl_string_new("path"));
  lcl_list_push(&result,
                lcl_string_new(buf->file_path ? buf->file_path : "(untitled)"));
  lcl_list_push(&result, lcl_string_new("length"));
  lcl_list_push(&result, lcl_int_new((long)document_length(doc)));
  lcl_list_push(&result, lcl_string_new("lines"));
  lcl_list_push(&result, lcl_int_new((long)document_line_count(doc)));
  lcl_list_push(&result, lcl_string_new("modified"));
  lcl_list_push(&result, lcl_int_new(buf->modified ? 1 : 0));

  *out = result;
  return LCL_RC_OK;
}

// Helper struct for finding pane by buffer
typedef struct {
  Buffer *target;
  Pane *found;
} FindPaneByBufferData;

// Callback for layout traversal to find pane by buffer
static void find_pane_by_buffer_cb(Pane *pane, void *data) {
  FindPaneByBufferData *d = data;
  if (d->found) {
    return; // Already found
  }
  if (pane_get_buffer(pane) == d->target) {
    d->found = pane;
  }
}

// Find pane containing a specific buffer
static Pane *find_pane_for_buffer(LclHost *host, Buffer *buffer) {
  if (!host || !host->layout_root_ptr || !*host->layout_root_ptr || !buffer) {
    return NULL;
  }
  FindPaneByBufferData data = {.target = buffer, .found = NULL};
  layout_for_each_pane(*host->layout_root_ptr, find_pane_by_buffer_cb, &data);
  return data.found;
}

// weft::repl::target-selection -> get selection from attached buffer
static int c_repl_target_selection(lcl_interp *interp, int argc,
                                   lcl_value **argv, lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (!host->repl_attached_buffer) {
    // Not attached - return empty list
    *out = lcl_list_new();
    return LCL_RC_OK;
  }

  // Find the pane containing the attached buffer
  Pane *pane = find_pane_for_buffer(host, host->repl_attached_buffer);
  if (!pane || !pane->view) {
    // Buffer exists but no pane - maybe it was closed
    *out = lcl_list_new();
    return LCL_RC_OK;
  }

  Editor *editor = pane->view;
  size_t sel_start, sel_end;
  editor_get_selection(editor, &sel_start, &sel_end);

  if (sel_start == sel_end) {
    // No selection - return empty list
    *out = lcl_list_new();
    return LCL_RC_OK;
  }

  // Get selection text
  size_t sel_len = sel_end - sel_start;
  char *text = malloc(sel_len + 1);
  if (!text) {
    lcl_set_error(interp, "allocation failed");
    return LCL_RC_ERR;
  }
  document_get_text(host->repl_attached_buffer->document, sel_start, text,
                    sel_len);
  text[sel_len] = '\0';

  // Return {start end text}
  lcl_value *result = lcl_list_new();
  lcl_list_push(&result, lcl_int_new((long)sel_start));
  lcl_list_push(&result, lcl_int_new((long)sel_end));
  lcl_list_push(&result, lcl_string_new(text));
  free(text);

  *out = result;
  return LCL_RC_OK;
}

// weft::repl::target-cursor -> get cursor position in attached buffer
static int c_repl_target_cursor(lcl_interp *interp, int argc, lcl_value **argv,
                                lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (!host->repl_attached_buffer) {
    *out = lcl_int_new(-1); // Not attached
    return LCL_RC_OK;
  }

  Pane *pane = find_pane_for_buffer(host, host->repl_attached_buffer);
  if (!pane || !pane->view) {
    *out = lcl_int_new(-1); // No pane
    return LCL_RC_OK;
  }

  size_t cursor = editor_get_cursor_pos(pane->view);
  *out = lcl_int_new((long)cursor);
  return LCL_RC_OK;
}

// weft::repl::goto pos -> move cursor in attached buffer
static int c_repl_goto(lcl_interp *interp, int argc, lcl_value **argv,
                       lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (!host->repl_attached_buffer) {
    lcl_set_error(interp, "not attached to a buffer");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "repl::goto requires position");
    return LCL_RC_ERR;
  }

  long pos;
  if (lcl_value_to_int(argv[0], &pos) != LCL_OK || pos < 0) {
    lcl_set_error(interp, "repl::goto: invalid position");
    return LCL_RC_ERR;
  }

  Pane *pane = find_pane_for_buffer(host, host->repl_attached_buffer);
  if (!pane || !pane->view) {
    lcl_set_error(interp, "target buffer has no pane");
    return LCL_RC_ERR;
  }

  // Clamp to document length
  size_t doc_len = document_length(host->repl_attached_buffer->document);
  if ((size_t)pos > doc_len) {
    pos = (long)doc_len;
  }

  editor_set_cursor_pos(pane->view, (size_t)pos);

  *out = lcl_int_new(pos);
  return LCL_RC_OK;
}

// === Selection namespace ===

// Helper: Get the effective selection context
// Returns the editor and buffer to use for selection queries.
// If in REPL and attached, uses the attached buffer; otherwise uses current.
// Returns false if no valid context.
static bool get_effective_selection_context(LclHost *host, Editor **out_editor,
                                            Buffer **out_buffer) {
  if (!host) {
    return false;
  }

  // Check if we're in the REPL and attached to a buffer
  if (host->current_buffer && is_repl_buffer(host->current_buffer) &&
      host->repl_attached_buffer) {
    // Use the attached buffer's selection
    Pane *pane = find_pane_for_buffer(host, host->repl_attached_buffer);
    if (pane && pane->view) {
      *out_editor = pane->view;
      *out_buffer = host->repl_attached_buffer;
      return true;
    }
  }

  // Use current editor/buffer
  if (host->current_editor && host->current_buffer) {
    *out_editor = host->current_editor;
    *out_buffer = host->current_buffer;
    return true;
  }

  return false;
}

// weft::sel::start -> get selection start (or cursor if no selection)
static int c_sel_start(lcl_interp *interp, int argc, lcl_value **argv,
                       lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  Editor *editor;
  Buffer *buffer;

  if (!get_effective_selection_context(host, &editor, &buffer)) {
    lcl_set_error(interp, "no selection context");
    return LCL_RC_ERR;
  }

  size_t start, end;
  editor_get_selection(editor, &start, &end);

  *out = lcl_int_new((long)start);
  return LCL_RC_OK;
}

// weft::sel::end -> get selection end (or cursor if no selection)
static int c_sel_end(lcl_interp *interp, int argc, lcl_value **argv,
                     lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  Editor *editor;
  Buffer *buffer;

  if (!get_effective_selection_context(host, &editor, &buffer)) {
    lcl_set_error(interp, "no selection context");
    return LCL_RC_ERR;
  }

  size_t start, end;
  editor_get_selection(editor, &start, &end);

  *out = lcl_int_new((long)end);
  return LCL_RC_OK;
}

// weft::sel::text -> get selected text (empty string if no selection)
static int c_sel_text(lcl_interp *interp, int argc, lcl_value **argv,
                      lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  Editor *editor;
  Buffer *buffer;

  if (!get_effective_selection_context(host, &editor, &buffer)) {
    lcl_set_error(interp, "no selection context");
    return LCL_RC_ERR;
  }

  size_t start, end;
  editor_get_selection(editor, &start, &end);

  if (start == end) {
    *out = lcl_string_new("");
    return LCL_RC_OK;
  }

  size_t len = end - start;
  char *text = malloc(len + 1);
  if (!text) {
    lcl_set_error(interp, "allocation failed");
    return LCL_RC_ERR;
  }

  document_get_text(buffer->document, start, text, len);
  text[len] = '\0';

  *out = lcl_string_new(text);
  free(text);
  return LCL_RC_OK;
}

// weft::sel::buffer -> get buffer ID of selection context
static int c_sel_buffer(lcl_interp *interp, int argc, lcl_value **argv,
                        lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  Editor *editor;
  Buffer *buffer;

  if (!get_effective_selection_context(host, &editor, &buffer)) {
    lcl_set_error(interp, "no selection context");
    return LCL_RC_ERR;
  }

  *out = lcl_int_new((long)buffer->id);
  return LCL_RC_OK;
}

// weft::sel::exists? -> check if there's a non-empty selection
static int c_sel_exists(lcl_interp *interp, int argc, lcl_value **argv,
                        lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  Editor *editor;
  Buffer *buffer;

  if (!get_effective_selection_context(host, &editor, &buffer)) {
    *out = lcl_int_new(0);
    return LCL_RC_OK;
  }

  size_t start, end;
  editor_get_selection(editor, &start, &end);

  *out = lcl_int_new(start != end ? 1 : 0);
  return LCL_RC_OK;
}

// weft::sel::info -> get full selection info as dict {start end buffer text}
static int c_sel_info(lcl_interp *interp, int argc, lcl_value **argv,
                      lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  Editor *editor;
  Buffer *buffer;

  if (!get_effective_selection_context(host, &editor, &buffer)) {
    // Return empty list if no context
    *out = lcl_list_new();
    return LCL_RC_OK;
  }

  size_t start, end;
  editor_get_selection(editor, &start, &end);

  // Get text if there's a selection
  char *text = NULL;
  if (start != end) {
    size_t len = end - start;
    text = malloc(len + 1);
    if (text) {
      document_get_text(buffer->document, start, text, len);
      text[len] = '\0';
    }
  }

  // Build result: #{start <n> end <n> buffer <id> text <str>}
  lcl_value *result = lcl_dict_new();
  lcl_dict_put(&result, "start", lcl_int_new((long)start));
  lcl_dict_put(&result, "end", lcl_int_new((long)end));
  lcl_dict_put(&result, "buffer", lcl_int_new((long)buffer->id));
  lcl_dict_put(&result, "text", lcl_string_new(text ? text : ""));

  free(text);
  *out = result;
  return LCL_RC_OK;
}

// weft::inspect::at pos -> show annotations and decorations at position in
// output buffer
static int c_inspect_at(lcl_interp *interp, int argc, lcl_value **argv,
                        lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  Buffer *buffer = get_effective_doc_buffer(host);
  if (!buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  if (argc < 1) {
    lcl_set_error(interp, "inspect::at requires position");
    return LCL_RC_ERR;
  }

  long pos;
  if (lcl_value_to_int(argv[0], &pos) != LCL_OK || pos < 0) {
    lcl_set_error(interp, "inspect::at: invalid position");
    return LCL_RC_ERR;
  }

  AnnotStore *store = buffer->annots;
  Document *doc = buffer->document;

  // Query all annotations at this position (no layer filter)
  AnnotQuery query = {0};
  annot_store_at(store, (size_t)pos, NULL, &query);

  // Try to get layer decorations from the focused pane
  const Decoration *decors[32];
  size_t decor_count = 0;
  Pane *pane = host->focused_pane_ptr ? *host->focused_pane_ptr : NULL;

  // If REPL is attached, find the pane for the attached buffer
  if (is_repl_buffer(host->current_buffer) && host->repl_attached_buffer) {
    // We need to find the pane for the attached buffer
    // For now, we can't easily do this, so skip decorations
    pane = NULL;
  }

  if (pane && pane->view && editor_get_buffer(pane->view) == buffer &&
      pane->layer_stack) {
    // Ensure decorations are rendered for this position
    size_t doc_len = document_length(doc);
    size_t vp_start = (size_t)pos > 256 ? (size_t)pos - 256 : 0;
    size_t vp_end = (size_t)pos + 256;
    if (vp_end > doc_len) {
      vp_end = doc_len;
    }

    layer_stack_render(pane->layer_stack, buffer, vp_start, vp_end);
    decor_count =
        layer_stack_decorations_at(pane->layer_stack, (size_t)pos, decors, 32);
  }

  // Build output string
  size_t output_cap = 8192;
  char *output = malloc(output_cap);
  if (!output) {
    annot_query_free(&query);
    lcl_set_error(interp, "inspect::at: allocation failed");
    return LCL_RC_ERR;
  }
  size_t output_len = 0;

  // Header
  output_len += snprintf(output + output_len, output_cap - output_len,
                         ";; Inspection at position %ld\n", pos);

  // Get line info for context
  size_t line_num = document_pos_to_line(doc, (size_t)pos);
  size_t line_start = document_line_start(doc, line_num);
  size_t col = (size_t)pos - line_start;
  output_len += snprintf(output + output_len, output_cap - output_len,
                         ";; Line %zu, Column %zu\n", line_num + 1, col + 1);

  if (query.count == 0) {
    output_len += snprintf(output + output_len, output_cap - output_len,
                           ";;\n;; No annotations found at this position.\n");
  } else {
    output_len +=
        snprintf(output + output_len, output_cap - output_len,
                 ";;\n;; Found %zu annotation(s):\n;;\n", query.count);

    for (size_t i = 0; i < query.count; i++) {
      uint32_t id = query.ids[i];
      const AnnotRecord *rec = annot_store_get(store, id);
      if (!rec) {
        continue;
      }

      // Get span positions
      size_t start_pos = 0, end_pos = 0;
      annot_store_get_span(store, id, &start_pos, &end_pos);

      // Extract the annotated text (up to 80 chars)
      size_t text_len = end_pos - start_pos;
      size_t preview_len = text_len > 80 ? 80 : text_len;
      char *text_preview = malloc(preview_len + 4); // +4 for "..." and null
      if (text_preview) {
        document_get_text(doc, start_pos, text_preview, preview_len);
        text_preview[preview_len] = '\0';
        // Replace newlines with visible markers
        for (size_t j = 0; j < preview_len; j++) {
          if (text_preview[j] == '\n') {
            text_preview[j] = ' ';
          }
        }
        if (text_len > 80) {
          strcat(text_preview, "...");
        }
      }

      // Ensure we have space - grow buffer if needed
      size_t needed = output_len + 1024 + (rec->meta_count * 256);
      if (needed > output_cap) {
        output_cap = needed * 2;
        char *new_output = realloc(output, output_cap);
        if (!new_output) {
          free(text_preview);
          free(output);
          annot_query_free(&query);
          lcl_set_error(interp, "inspect::at: allocation failed");
          return LCL_RC_ERR;
        }
        output = new_output;
      }

      // Find provenance info first
      const char *prov_script = NULL;
      for (size_t j = 0; j < rec->meta_count; j++) {
        if (strcmp(rec->keys[j], "prov:script") == 0) {
          prov_script = rec->values[j];
          break;
        }
      }

      // Annotation header
      output_len +=
          snprintf(output + output_len, output_cap - output_len,
                   "─────────────────────────────────────────\n"
                   "Annotation #%u\n"
                   "  Layer: %s\n"
                   "  Span:  [%zu, %zu) (%zu bytes)\n"
                   "  Text:  \"%s\"\n",
                   id, rec->layer ? rec->layer : "(none)", start_pos, end_pos,
                   text_len, text_preview ? text_preview : "(error)");

      // Show provenance if available
      if (prov_script) {
        output_len += snprintf(output + output_len, output_cap - output_len,
                               "  Source: %s\n", prov_script);
      }

      free(text_preview);

      // Metadata (excluding prov:* keys)
      bool has_non_prov_meta = false;
      for (size_t j = 0; j < rec->meta_count; j++) {
        if (strncmp(rec->keys[j], "prov:", 5) != 0) {
          has_non_prov_meta = true;
          break;
        }
      }

      if (has_non_prov_meta) {
        output_len += snprintf(output + output_len, output_cap - output_len,
                               "  Metadata:\n");
        for (size_t j = 0; j < rec->meta_count; j++) {
          // Skip provenance keys - they're shown separately
          if (strncmp(rec->keys[j], "prov:", 5) == 0) {
            continue;
          }
          output_len +=
              snprintf(output + output_len, output_cap - output_len,
                       "    %s = \"%s\"\n", rec->keys[j], rec->values[j]);
        }
      }

      output_len +=
          snprintf(output + output_len, output_cap - output_len, "\n");
    }
  }

  // Display layer decorations (ephemeral styling from ViewLayers)
  if (decor_count > 0) {
    // Ensure we have space for decorations
    size_t needed = output_len + 512 + (decor_count * 512);
    if (needed > output_cap) {
      output_cap = needed * 2;
      char *new_output = realloc(output, output_cap);
      if (!new_output) {
        free(output);
        annot_query_free(&query);
        lcl_set_error(interp, "inspect::at: allocation failed");
        return LCL_RC_ERR;
      }
      output = new_output;
    }

    output_len +=
        snprintf(output + output_len, output_cap - output_len,
                 ";;\n;; Found %zu layer decoration(s):\n;;\n", decor_count);

    for (size_t i = 0; i < decor_count; i++) {
      const Decoration *d = decors[i];

      // Extract the decorated text (up to 80 chars)
      size_t text_len = d->end - d->start;
      size_t preview_len = text_len > 80 ? 80 : text_len;
      char *text_preview = malloc(preview_len + 4);
      if (text_preview) {
        document_get_text(doc, d->start, text_preview, preview_len);
        text_preview[preview_len] = '\0';
        for (size_t j = 0; j < preview_len; j++) {
          if (text_preview[j] == '\n') {
            text_preview[j] = ' ';
          }
        }
        if (text_len > 80) {
          strcat(text_preview, "...");
        }
      }

      output_len +=
          snprintf(output + output_len, output_cap - output_len,
                   "─────────────────────────────────────────\n"
                   "Decoration (ephemeral)\n"
                   "  Layer: %s\n"
                   "  Span:  [%zu, %zu) (%zu bytes)\n"
                   "  Text:  \"%s\"\n",
                   d->layer_name ? d->layer_name : "(none)", d->start, d->end,
                   text_len, text_preview ? text_preview : "(error)");

      // Show style info
      if (d->has_fg || d->has_bg || d->underline) {
        output_len += snprintf(output + output_len, output_cap - output_len,
                               "  Style:\n");
        if (d->has_fg) {
          output_len +=
              snprintf(output + output_len, output_cap - output_len,
                       "    fg = #%02X%02X%02X\n", d->fg.r, d->fg.g, d->fg.b);
        }
        if (d->has_bg) {
          output_len +=
              snprintf(output + output_len, output_cap - output_len,
                       "    bg = #%02X%02X%02X\n", d->bg.r, d->bg.g, d->bg.b);
        }
        if (d->underline) {
          output_len += snprintf(output + output_len, output_cap - output_len,
                                 "    underline = true\n");
        }
      }

      // Show action if present
      if (d->action_script) {
        output_len += snprintf(output + output_len, output_cap - output_len,
                               "  Action: %s\n", d->action_script);
      }

      free(text_preview);
      output_len +=
          snprintf(output + output_len, output_cap - output_len, "\n");
    }
  } else if (query.count == 0) {
    // Only show "no decorations" if also no annotations
    // (already shown "no annotations" above)
  }

  annot_query_free(&query);

  // Open output buffer with the result
  if (!host->buffer_manager || !host->layout_root_ptr) {
    // No buffer manager - just return the output as string
    *out = lcl_string_new(output);
    free(output);
    return LCL_RC_OK;
  }

  // Create new buffer
  Buffer *new_buffer = buffer_manager_create(host->buffer_manager);
  if (!new_buffer) {
    free(output);
    lcl_set_error(interp, "inspect::at: failed to create buffer");
    return LCL_RC_ERR;
  }

  // Set content
  document_insert(new_buffer->document, 0, output, output_len);
  free(output);

  // Set display name
  char display_name[64];
  snprintf(display_name, sizeof(display_name), "*inspect@%ld*", pos);
  new_buffer->file_path = strdup(display_name);

  // Create new pane
  Pane *new_pane = pane_new(new_buffer, host->char_width, host->line_height);
  if (!new_pane) {
    buffer_manager_close(host->buffer_manager, new_buffer);
    lcl_set_error(interp, "inspect::at: failed to create pane");
    return LCL_RC_ERR;
  }

  // Apply viewlayer configuration
  lcl_host_apply_viewlayers(host, new_pane);

  // Split to show new pane
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  // Recompute layout bounds
  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::buffer::open-at path line -> open file and jump to line
static int c_buffer_open_at(lcl_interp *interp, int argc, lcl_value **argv,
                            lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no buffer manager context");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp, "buffer::open-at requires path and line");
    return LCL_RC_ERR;
  }

  const char *path = lcl_value_to_string(argv[0]);
  if (!path) {
    lcl_set_error(interp, "buffer::open-at: path must be a string");
    return LCL_RC_ERR;
  }

  long line_num;
  if (lcl_value_to_int(argv[1], &line_num) != LCL_OK || line_num < 1) {
    lcl_set_error(interp, "buffer::open-at: line must be a positive integer");
    return LCL_RC_ERR;
  }

  // Open file in buffer manager
  Buffer *new_buffer = buffer_manager_open_file(host->buffer_manager, path);
  if (!new_buffer) {
    lcl_set_error(interp, "buffer::open-at: failed to open file");
    return LCL_RC_ERR;
  }

  // Apply file type associations (syntax highlighting, etc.)
  lcl_host_apply_filetypes(host, new_buffer);

  // Create a new pane for this buffer using renderer metrics
  Pane *new_pane = pane_new(new_buffer, host->char_width, host->line_height);
  if (!new_pane) {
    buffer_manager_close(host->buffer_manager, new_buffer);
    lcl_set_error(interp, "buffer::open-at: failed to create pane");
    return LCL_RC_ERR;
  }

  // Apply viewlayer configuration based on file type
  lcl_host_apply_viewlayers(host, new_pane);

  // Apply indent settings based on file type
  Editor *editor = pane_get_editor(new_pane);
  lcl_host_apply_indent(host, editor);

  // Apply wrap settings based on file type
  lcl_host_apply_wrap(host, editor);

  // Split current pane horizontally to show new pane
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  // Recompute layout bounds
  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }

  // Jump to the specified line (1-indexed)
  if (editor && new_buffer->document) {
    // Convert 1-indexed line to 0-indexed
    size_t line_idx = (size_t)(line_num - 1);
    size_t line_count = document_line_count(new_buffer->document);

    // Clamp to valid range
    if (line_idx >= line_count) {
      line_idx = line_count > 0 ? line_count - 1 : 0;
    }

    // Get byte position of line start
    size_t pos = document_line_start(new_buffer->document, line_idx);
    editor->cursor_pos = pos;
    editor->selection_anchor = SIZE_MAX; // Clear any selection
    editor_scroll_to_cursor(editor);
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// === Internal: Pane Commands ===

// Helper to close the focused pane - returns true if closed, false if last pane
static bool close_focused_pane(LclHost *host) {
  if (!host || !host->layout_root_ptr || !host->focused_pane_ptr) {
    return false;
  }

  LayoutNode *root = *host->layout_root_ptr;
  Pane *focused = *host->focused_pane_ptr;

  if (!root || !focused) {
    return false;
  }

  // Find the next pane to focus before closing
  Pane *next = layout_next_pane(root, focused);
  if (next == focused) {
    next = NULL; // Only one pane
  }

  // Close the pane
  LayoutNode *new_root = layout_close_pane(root, focused);

  if (new_root) {
    *host->layout_root_ptr = new_root;
    *host->focused_pane_ptr = next ? next : layout_first_pane(new_root);
    recompute_layout_bounds(host);
    return true;
  } else {
    // Last pane closed
    *host->layout_root_ptr = NULL;
    *host->focused_pane_ptr = NULL;
    return false;
  }
}

// Helper to kill the focused pane (close pane AND destroy buffer)
// Used for temporary buffers like *diff* and *annot-edit*
static bool kill_focused_pane(LclHost *host) {
  if (!host || !host->layout_root_ptr || !host->focused_pane_ptr ||
      !host->buffer_manager) {
    return false;
  }

  Pane *focused = *host->focused_pane_ptr;
  if (!focused) {
    return false;
  }

  Buffer *buf = pane_get_buffer(focused);
  if (!buf) {
    return close_focused_pane(host);
  }

  // Clear associated state before destroying
  if (host->annot_edit.target_buffer == buf) {
    annot_edit_clear(host);
  }
  if (buf->file_path && strcmp(buf->file_path, "*annot-edit*") == 0) {
    annot_edit_clear(host);
  }
  if (host->patch_target_buffer == buf) {
    if (host->pending_patch) {
      patch_free(host->pending_patch);
      host->pending_patch = NULL;
    }
    host->patch_target_buffer = NULL;
  }
  if (buf->file_path && strcmp(buf->file_path, "*diff*") == 0) {
    if (host->pending_patch) {
      patch_free(host->pending_patch);
      host->pending_patch = NULL;
    }
    host->patch_target_buffer = NULL;
  }
  if (host->repl_attached_buffer == buf) {
    host->repl_attached_buffer = NULL;
  }
  if (host->current_buffer == buf) {
    host->current_buffer = NULL;
    host->current_editor = NULL;
  }

  // Close the pane first
  LayoutNode *root = *host->layout_root_ptr;
  Pane *next = layout_next_pane(root, focused);
  if (next == focused) {
    next = NULL;
  }

  LayoutNode *new_root = layout_close_pane(root, focused);

  if (new_root) {
    *host->layout_root_ptr = new_root;
    *host->focused_pane_ptr = next ? next : layout_first_pane(new_root);
    recompute_layout_bounds(host);
  } else {
    // Last pane - don't kill
    return false;
  }

  // Destroy the buffer
  buffer_manager_close(host->buffer_manager, buf);

  return true;
}

// weft::pane::close -> close the currently focused pane
static int c_pane_close(lcl_interp *interp, int argc, lcl_value **argv,
                        lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->layout_root_ptr || !host->focused_pane_ptr) {
    lcl_set_error(interp, "no app context");
    return LCL_RC_ERR;
  }

  bool closed = close_focused_pane(host);
  *out = lcl_int_new(closed ? 1 : 0);
  return LCL_RC_OK;
}

// weft::pane::next -> focus the next pane (cycle)
static int c_pane_next(lcl_interp *interp, int argc, lcl_value **argv,
                       lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->layout_root_ptr || !host->focused_pane_ptr) {
    lcl_set_error(interp, "no app context");
    return LCL_RC_ERR;
  }

  LayoutNode *root = *host->layout_root_ptr;
  Pane *focused = *host->focused_pane_ptr;

  if (!root || !focused) {
    *out = lcl_int_new(0);
    return LCL_RC_OK;
  }

  Pane *next = layout_next_pane(root, focused);
  if (next && next != focused) {
    *host->focused_pane_ptr = next;
    *out = lcl_int_new(1); // Changed focus
  } else {
    *out = lcl_int_new(0); // No other pane
  }

  return LCL_RC_OK;
}

// weft::pane::prev -> focus the previous pane (cycle backward)
static int c_pane_prev(lcl_interp *interp, int argc, lcl_value **argv,
                       lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->layout_root_ptr || !host->focused_pane_ptr) {
    lcl_set_error(interp, "no app context");
    return LCL_RC_ERR;
  }

  LayoutNode *root = *host->layout_root_ptr;
  Pane *focused = *host->focused_pane_ptr;

  if (!root || !focused) {
    *out = lcl_int_new(0);
    return LCL_RC_OK;
  }

  Pane *prev = layout_prev_pane(root, focused);
  if (prev && prev != focused) {
    *host->focused_pane_ptr = prev;
    *out = lcl_int_new(1); // Changed focus
  } else {
    *out = lcl_int_new(0); // No other pane
  }

  return LCL_RC_OK;
}

// weft::viewlayer::add type param ?priority? -> add a ViewLayer to current pane
// type: "annot" (annotation store layer)
// param: layer name for annot type
// priority: render order (higher = on top), default 50
static int c_viewlayer_add(lcl_interp *interp, int argc, lcl_value **argv,
                           lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp, "viewlayer::add requires type and param");
    return LCL_RC_ERR;
  }

  const char *type_str = lcl_value_to_string(argv[0]);
  const char *param = lcl_value_to_string(argv[1]);

  if (!type_str || !param) {
    lcl_set_error(interp, "viewlayer::add: type and param must be strings");
    return LCL_RC_ERR;
  }

  int priority = 50;
  if (argc >= 3) {
    long prio;
    if (lcl_value_to_int(argv[2], &prio) == LCL_OK) {
      priority = (int)prio;
    }
  }

  // Get the focused pane
  if (!host->focused_pane_ptr || !*host->focused_pane_ptr) {
    lcl_set_error(interp, "viewlayer::add: no focused pane");
    return LCL_RC_ERR;
  }

  Pane *pane = *host->focused_pane_ptr;
  if (!pane->layer_stack) {
    lcl_set_error(interp, "viewlayer::add: pane has no layer stack");
    return LCL_RC_ERR;
  }

  ViewLayer *layer = NULL;

  if (strcmp(type_str, "annot") == 0) {
    layer = annot_layer_new(param, priority);
  } else {
    lcl_set_error(interp, "viewlayer::add: unsupported type (use 'annot')");
    return LCL_RC_ERR;
  }

  if (!layer) {
    lcl_set_error(interp, "viewlayer::add: failed to create layer");
    return LCL_RC_ERR;
  }

  layer_stack_add(pane->layer_stack, layer);

  *out = lcl_int_new(1);
  return LCL_RC_OK;
}

// === Internal: Filesystem Bindings ===

// Helper: Compare directory entries for sorting (directories first, then alpha)
static int dirent_cmp(const void *a, const void *b) {
  const struct dirent **da = (const struct dirent **)a;
  const struct dirent **db = (const struct dirent **)b;

  // Directories come first
  bool a_is_dir = ((*da)->d_type == DT_DIR);
  bool b_is_dir = ((*db)->d_type == DT_DIR);

  if (a_is_dir && !b_is_dir) {
    return -1;
  }
  if (!a_is_dir && b_is_dir) {
    return 1;
  }

  // Then alphabetical
  return strcmp((*da)->d_name, (*db)->d_name);
}

// weft::fs::ls ?path? -> list directory, open annotated buffer
static int c_fs_ls(lcl_interp *interp, int argc, lcl_value **argv,
                   lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);

  if (!host || !host->buffer_manager || !host->layout_root_ptr) {
    lcl_set_error(interp, "no app context");
    return LCL_RC_ERR;
  }

  // Get path (default to current directory)
  char path[PATH_MAX];

  if (argc > 0) {
    const char *arg = lcl_value_to_string(argv[0]);

    if (arg[0] == '/') {
      snprintf(path, sizeof(path), "%s", arg);
    } else {
      // Relative path - resolve from cwd
      if (getcwd(path, sizeof(path)) == NULL) {
        lcl_set_error(interp, "fs::ls: cannot get cwd");
        return LCL_RC_ERR;
      }

      size_t len = strlen(path);
      snprintf(path + len, sizeof(path) - len, "/%s", arg);
    }
  } else {
    if (getcwd(path, sizeof(path)) == NULL) {
      lcl_set_error(interp, "fs::ls: cannot get cwd");
      return LCL_RC_ERR;
    }
  }

  // Normalize path (remove trailing slash unless root)
  size_t plen = strlen(path);

  if (plen > 1 && path[plen - 1] == '/') {
    path[plen - 1] = '\0';
  }

  // Open directory
  DIR *dir = opendir(path);

  if (!dir) {
    lcl_set_error(interp, "fs::ls: cannot open directory");
    return LCL_RC_ERR;
  }

  // Collect entries
  struct dirent **entries = NULL;
  size_t entry_count = 0;
  size_t entry_cap = 64;
  entries = malloc(entry_cap * sizeof(struct dirent *));

  if (!entries) {
    closedir(dir);
    lcl_set_error(interp, "out of memory");
    return LCL_RC_ERR;
  }

  struct dirent *ent;

  while ((ent = readdir(dir)) != NULL) {
    // Skip . and ..
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    // Grow array if needed
    if (entry_count >= entry_cap) {
      entry_cap *= 2;
      struct dirent **new_entries =
          realloc(entries, entry_cap * sizeof(struct dirent *));
      if (!new_entries) {
        for (size_t i = 0; i < entry_count; i++) {
          free(entries[i]);
        }
        free(entries);
        closedir(dir);
        lcl_set_error(interp, "out of memory");
        return LCL_RC_ERR;
      }
      entries = new_entries;
    }

    // Copy entry
    entries[entry_count] = malloc(sizeof(struct dirent));
    if (!entries[entry_count]) {
      for (size_t i = 0; i < entry_count; i++) {
        free(entries[i]);
      }
      free(entries);
      closedir(dir);
      lcl_set_error(interp, "out of memory");
      return LCL_RC_ERR;
    }
    memcpy(entries[entry_count], ent, sizeof(struct dirent));
    entry_count++;
  }
  closedir(dir);

  // Sort entries
  qsort(entries, entry_count, sizeof(struct dirent *), dirent_cmp);

  // Build content string and track positions for annotations
  // Format: "name\n" for files, "name/\n" for directories
  size_t content_cap = 4096;
  char *content = malloc(content_cap);

  if (!content) {
    for (size_t i = 0; i < entry_count; i++) {
      free(entries[i]);
    }
    free(entries);
    lcl_set_error(interp, "out of memory");
    return LCL_RC_ERR;
  }
  size_t content_len = 0;

  // Track annotation info: start, end, full_path, is_dir for each entry
  typedef struct {
    size_t start;
    size_t end;
    char full_path[PATH_MAX];
    bool is_dir;
  } EntryInfo;

  EntryInfo *entry_info = malloc(entry_count * sizeof(EntryInfo));

  if (!entry_info) {
    free(content);

    for (size_t i = 0; i < entry_count; i++) {
      free(entries[i]);
    }

    free(entries);
    lcl_set_error(interp, "out of memory");
    return LCL_RC_ERR;
  }

  for (size_t i = 0; i < entry_count; i++) {
    const char *name = entries[i]->d_name;
    bool is_dir = (entries[i]->d_type == DT_DIR);

    // If d_type is unknown, stat to check
    if (entries[i]->d_type == DT_UNKNOWN) {
      char full[PATH_MAX];
      snprintf(full, sizeof(full), "%s/%s", path, name);
      struct stat st;

      if (stat(full, &st) == 0) {
        is_dir = S_ISDIR(st.st_mode);
      }
    }

    size_t name_len = strlen(name);
    size_t line_len = name_len + (is_dir ? 2 : 1); // +1 for \n, +1 for / if dir

    // Grow content buffer if needed
    while (content_len + line_len + 1 > content_cap) {
      content_cap *= 2;
      char *new_content = realloc(content, content_cap);
      if (!new_content) {
        free(content);
        free(entry_info);

        for (size_t j = 0; j < entry_count; j++) {
          free(entries[j]);
        }

        free(entries);
        lcl_set_error(interp, "out of memory");
        return LCL_RC_ERR;
      }
      content = new_content;
    }

    // Record position
    entry_info[i].start = content_len;
    entry_info[i].is_dir = is_dir;
    snprintf(entry_info[i].full_path, sizeof(entry_info[i].full_path), "%s/%s",
             path, name);

    // Append name
    memcpy(content + content_len, name, name_len);
    content_len += name_len;

    if (is_dir) {
      content[content_len++] = '/';
    }

    entry_info[i].end = content_len; // End before newline

    content[content_len++] = '\n';
  }
  content[content_len] = '\0';

  // Free entries array (we have the info we need)
  for (size_t i = 0; i < entry_count; i++) {
    free(entries[i]);
  }
  free(entries);

  // Create new buffer
  Buffer *new_buffer = buffer_manager_create(host->buffer_manager);
  if (!new_buffer) {
    free(content);
    free(entry_info);
    lcl_set_error(interp, "fs::ls: failed to create buffer");
    return LCL_RC_ERR;
  }

  // Set content
  if (content_len > 0) {
    document_insert(new_buffer->document, 0, content, content_len);
  }

  // Set display name
  char display_name[PATH_MAX + 8];
  snprintf(display_name, sizeof(display_name), "+ls %s", path);
  new_buffer->file_path = strdup(display_name);

  // Add annotations for each entry
  for (size_t i = 0; i < entry_count; i++) {
    // Create handler script (must call handled to stop further dispatch)
    char handler[PATH_MAX + 128];
    if (entry_info[i].is_dir) {
      // Directory: ls it
      snprintf(handler, sizeof(handler),
               "weft::fs::ls \"%s\"\nweft::interact::handled",
               entry_info[i].full_path);
    } else {
      // File: open it
      snprintf(handler, sizeof(handler),
               "weft::buffer::open \"%s\"\nweft::interact::handled",
               entry_info[i].full_path);
    }

    // Add annotation with action handler
    const char *keys[] = {"action:on_middle_click", "fs:path", "fs:type"};
    const char *vals[] = {handler, entry_info[i].full_path,
                          entry_info[i].is_dir ? "directory" : "file"};

    annot_store_add(new_buffer->annots, entry_info[i].start, entry_info[i].end,
                    "action", keys, vals, 3);
  }

  free(content);
  free(entry_info);

  // Create new pane
  Pane *new_pane = pane_new(new_buffer, host->char_width, host->line_height);

  if (!new_pane) {
    buffer_manager_close(host->buffer_manager, new_buffer);
    lcl_set_error(interp, "fs::ls: failed to create pane");
    return LCL_RC_ERR;
  }

  // Apply viewlayer configuration
  lcl_host_apply_viewlayers(host, new_pane);

  // Split to show new pane
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  // Recompute layout bounds
  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// === Internal: Plumber Bindings ===

// Helper: Check if character can be part of a filename
static bool is_filename_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
}

// Helper: Check if character is a digit
static bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

// Internal helper: Scan buffer for file:line patterns and add annotations
// Returns number of annotations added
static int plumb_scan_buffer(Buffer *buf) {
  if (!buf || !buf->document || !buf->annots) {
    return 0;
  }

  Document *doc = buf->document;
  size_t doc_len = document_length(doc);

  if (doc_len == 0) {
    return 0;
  }

  // Read entire document into memory for scanning
  char *text = malloc(doc_len + 1);
  if (!text) {
    return 0;
  }
  document_get_text(doc, 0, text, doc_len);
  text[doc_len] = '\0';

  int annotations_added = 0;

  // Scan for file:line patterns
  // Pattern: <filename>.<ext>:<number> or <filename>.<ext>:<number>:<number>
  // Where filename can contain letters, digits, _, -, ., /
  size_t i = 0;
  while (i < doc_len) {
    // Skip non-filename characters
    while (i < doc_len && !is_filename_char(text[i])) {
      i++;
    }
    if (i >= doc_len) {
      break;
    }

    size_t file_start = i;

    // Scan filename (must contain at least one '.' for extension)
    bool has_dot = false;
    while (i < doc_len && is_filename_char(text[i])) {
      if (text[i] == '.') {
        has_dot = true;
      }
      i++;
    }
    size_t file_end = i;

    // Check for :number pattern
    if (i < doc_len && text[i] == ':' && has_dot) {
      i++;

      // Must have at least one digit
      if (i < doc_len && is_digit(text[i])) {
        size_t line_start = i;
        while (i < doc_len && is_digit(text[i])) {
          i++;
        }
        size_t line_end = i;

        // Extract line number
        char line_str[32];
        size_t line_len = line_end - line_start;
        if (line_len < sizeof(line_str)) {
          memcpy(line_str, text + line_start, line_len);
          line_str[line_len] = '\0';
          int line_num = atoi(line_str);

          // Check for optional :column
          if (i < doc_len && text[i] == ':' && i + 1 < doc_len &&
              is_digit(text[i + 1])) {
            i++; // skip :
            while (i < doc_len && is_digit(text[i])) {
              i++; // skip column digits
            }
          }

          // Extract filename
          size_t fname_len = file_end - file_start;
          char *filename = malloc(fname_len + 1);
          if (filename) {
            memcpy(filename, text + file_start, fname_len);
            filename[fname_len] = '\0';

            // Create handler script (must call handled to stop further
            // dispatch)
            char handler[PATH_MAX + 256];
            snprintf(handler, sizeof(handler),
                     "weft::buffer::open-at \"%s\" %d\nweft::interact::handled",
                     filename, line_num);

            // Add annotation spanning file:line
            const char *keys[] = {"action:on_middle_click", "plumb:file",
                                  "plumb:line"};
            char line_val[32];
            snprintf(line_val, sizeof(line_val), "%d", line_num);
            const char *vals[] = {handler, filename, line_val};

            annot_store_add(buf->annots, file_start, line_end, "action", keys,
                            vals, 3);
            annotations_added++;

            free(filename);
          }
        }
      }
    }
    // Continue scanning from current position
  }

  free(text);
  return annotations_added;
}

// weft::plumb::scan -> scan current buffer for patterns, add annotations
// Patterns recognized:
//   file.ext:line       -> open file at line
//   file.ext:line:col   -> open file at line (col ignored for now)
//   http://... https:// -> URLs (future)
static int c_plumb_scan(lcl_interp *interp, int argc, lcl_value **argv,
                        lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_buffer) {
    lcl_set_error(interp, "no buffer context");
    return LCL_RC_ERR;
  }

  int count = plumb_scan_buffer(host->current_buffer);
  *out = lcl_int_new(count);
  return LCL_RC_OK;
}

// === Internal: Config Bindings ===

// weft::config::filetype ext layer script-path -> register file type
// association
static int c_config_filetype(lcl_interp *interp, int argc, lcl_value **argv,
                             lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (argc < 3) {
    lcl_set_error(
        interp,
        "config::filetype requires extension, layer-name, and script-path");
    return LCL_RC_ERR;
  }

  const char *ext = lcl_value_to_string(argv[0]);
  const char *layer = lcl_value_to_string(argv[1]);
  const char *path = lcl_value_to_string(argv[2]);

  if (!ext || !layer || !path) {
    lcl_set_error(interp, "config::filetype: all arguments must be strings");
    return LCL_RC_ERR;
  }

  lcl_host_register_filetype(host, ext, layer, path);

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::config::handler name type script -> register global handler
static int c_config_handler(lcl_interp *interp, int argc, lcl_value **argv,
                            lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (argc < 3) {
    lcl_set_error(interp, "config::handler requires name, type, and script");
    return LCL_RC_ERR;
  }

  const char *name = lcl_value_to_string(argv[0]);
  const char *type_str = lcl_value_to_string(argv[1]);
  const char *script = lcl_value_to_string(argv[2]);

  if (!name || !type_str || !script) {
    lcl_set_error(interp, "config::handler: all arguments must be strings");
    return LCL_RC_ERR;
  }

  // Parse event type
  HandlerEventType event_type;
  if (strcmp(type_str, "left-click") == 0) {
    event_type = HANDLER_LEFT_CLICK;
  } else if (strcmp(type_str, "middle-click") == 0) {
    event_type = HANDLER_MIDDLE_CLICK;
  } else if (strcmp(type_str, "right-click") == 0) {
    event_type = HANDLER_RIGHT_CLICK;
  } else if (strcmp(type_str, "hover") == 0) {
    event_type = HANDLER_HOVER;
  } else {
    lcl_set_error(interp, "config::handler: unknown event type");
    return LCL_RC_ERR;
  }

  lcl_host_register_handler(host, name, event_type, script, 0);

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::config::dir -> return config directory path
static int c_config_dir(lcl_interp *interp, int argc, lcl_value **argv,
                        lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->config_dir) {
    *out = lcl_string_new("");
    return LCL_RC_OK;
  }

  *out = lcl_string_new(host->config_dir);
  return LCL_RC_OK;
}

// weft::config::indent ext tab-size use-tabs -> register indent settings
// use-tabs: 0 = spaces, non-zero = tabs
static int c_config_indent(lcl_interp *interp, int argc, lcl_value **argv,
                           lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (argc < 3) {
    lcl_set_error(interp,
                  "config::indent requires extension, tab-size, and use-tabs");
    return LCL_RC_ERR;
  }

  const char *ext = lcl_value_to_string(argv[0]);
  if (!ext) {
    lcl_set_error(interp, "config::indent: extension must be a string");
    return LCL_RC_ERR;
  }

  int64_t tab_size = 0;
  if (lcl_value_to_int(argv[1], &tab_size) != LCL_OK) {
    lcl_set_error(interp, "config::indent: tab-size must be an integer");
    return LCL_RC_ERR;
  }

  int64_t use_tabs = 0;
  if (lcl_value_to_int(argv[2], &use_tabs) != LCL_OK) {
    lcl_set_error(interp,
                  "config::indent: use-tabs must be an integer (0 or 1)");
    return LCL_RC_ERR;
  }

  lcl_host_register_indent(host, ext, (int)tab_size, use_tabs != 0);

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::config::wrap ext enabled wrap-width -> register wrap settings
static int c_config_wrap(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (argc < 3) {
    lcl_set_error(interp,
                  "config::wrap requires extension, enabled, and wrap-width");
    return LCL_RC_ERR;
  }

  const char *ext = lcl_value_to_string(argv[0]);
  if (!ext) {
    lcl_set_error(interp, "config::wrap: extension must be a string");
    return LCL_RC_ERR;
  }

  int64_t enabled = 0;
  if (lcl_value_to_int(argv[1], &enabled) != LCL_OK) {
    lcl_set_error(interp, "config::wrap: enabled must be an integer (0 or 1)");
    return LCL_RC_ERR;
  }

  int64_t wrap_width = 0;
  if (lcl_value_to_int(argv[2], &wrap_width) != LCL_OK) {
    lcl_set_error(interp, "config::wrap: wrap-width must be an integer");
    return LCL_RC_ERR;
  }

  lcl_host_register_wrap(host, ext, enabled != 0, (int)wrap_width);

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::config::font path size -> set global font configuration
static int c_config_font(lcl_interp *interp, int argc, lcl_value **argv,
                         lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp, "config::font requires path and size");
    return LCL_RC_ERR;
  }

  const char *path = lcl_value_to_string(argv[0]);
  if (!path) {
    lcl_set_error(interp, "config::font: path must be a string");
    return LCL_RC_ERR;
  }

  int64_t size = 0;
  if (lcl_value_to_int(argv[1], &size) != LCL_OK) {
    lcl_set_error(interp, "config::font: size must be an integer");
    return LCL_RC_ERR;
  }

  if (size < 6 || size > 72) {
    lcl_set_error(interp, "config::font: size must be between 6 and 72");
    return LCL_RC_ERR;
  }

  lcl_host_set_font(host, path, (int)size);

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::config::viewlayer ext type param ?priority? -> register viewlayer
// config type: "tree-sitter", "lcl-script", or "annot" param: language name
// (for tree-sitter), script path (for lcl-script), or
//        layer name (for annot)
static int c_config_viewlayer(lcl_interp *interp, int argc, lcl_value **argv,
                              lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (argc < 3) {
    lcl_set_error(interp,
                  "config::viewlayer requires extension, type, and param");
    return LCL_RC_ERR;
  }

  const char *ext = lcl_value_to_string(argv[0]);
  const char *type_str = lcl_value_to_string(argv[1]);
  const char *param = lcl_value_to_string(argv[2]);

  if (!ext || !type_str || !param) {
    lcl_set_error(
        interp,
        "config::viewlayer: extension, type, and param must be strings");
    return LCL_RC_ERR;
  }

  // Parse type
  ViewLayerType type;
  if (strcmp(type_str, "tree-sitter") == 0) {
    type = VIEWLAYER_TREE_SITTER;
  } else if (strcmp(type_str, "lcl-script") == 0) {
    type = VIEWLAYER_LCL_SCRIPT;
  } else if (strcmp(type_str, "annot") == 0) {
    type = VIEWLAYER_ANNOT;
  } else {
    lcl_set_error(
        interp, "config::viewlayer: type must be 'tree-sitter', 'lcl-script', "
                "or 'annot'");
    return LCL_RC_ERR;
  }

  // Optional priority (default 0)
  int priority = 0;
  if (argc >= 4) {
    int64_t prio = 0;
    if (lcl_value_to_int(argv[3], &prio) == LCL_OK) {
      priority = (int)prio;
    }
  }

  lcl_host_register_viewlayer(host, ext, type, param, priority);

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::config::language name library_path ?query_path? -> register tree-sitter
// language Registers a tree-sitter language that can be loaded dynamically.
// name: language identifier (e.g., "c", "javascript")
// library_path: path to shared library (.so/.dylib/.dll)
// query_path: optional path to highlights.scm (defaults to
// <library_dir>/queries/highlights.scm)
static int c_config_language(lcl_interp *interp, int argc, lcl_value **argv,
                             lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp, "config::language requires name and library_path");
    return LCL_RC_ERR;
  }

  const char *name = lcl_value_to_string(argv[0]);
  const char *library_path = lcl_value_to_string(argv[1]);
  const char *query_path = (argc >= 3) ? lcl_value_to_string(argv[2]) : NULL;

  if (!name || !library_path) {
    lcl_set_error(interp,
                  "config::language: name and library_path must be strings");
    return LCL_RC_ERR;
  }

  if (!ts_lang_registry_add_dynamic(host->ts_registry, name, library_path,
                                    query_path)) {
    lcl_set_error(interp, "config::language: failed to register language");
    return LCL_RC_ERR;
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::config::language-ext extension language_name -> map extension to
// language Maps a file extension to a registered tree-sitter language.
static int c_config_language_ext(lcl_interp *interp, int argc, lcl_value **argv,
                                 lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp,
                  "config::language-ext requires extension and language_name");
    return LCL_RC_ERR;
  }

  const char *extension = lcl_value_to_string(argv[0]);
  const char *language_name = lcl_value_to_string(argv[1]);

  if (!extension || !language_name) {
    lcl_set_error(
        interp,
        "config::language-ext: extension and language_name must be strings");
    return LCL_RC_ERR;
  }

  if (!ts_lang_registry_map_ext(host->ts_registry, extension, language_name)) {
    lcl_set_error(interp, "config::language-ext: failed to map extension");
    return LCL_RC_ERR;
  }

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// Helper to parse a key name to SDL keycode
static int parse_keycode(const char *name) {
  // Common single-character keys
  if (strlen(name) == 1) {
    char c = name[0];
    if (c >= 'a' && c <= 'z') {
      return SDLK_A + (c - 'a');
    }
    if (c >= 'A' && c <= 'Z') {
      return SDLK_A + (c - 'A');
    }
    if (c >= '0' && c <= '9') {
      return SDLK_0 + (c - '0');
    }
    // Special single chars
    switch (c) {
    case '`': return SDLK_GRAVE;
    case '-': return SDLK_MINUS;
    case '=': return SDLK_EQUALS;
    case '[': return SDLK_LEFTBRACKET;
    case ']': return SDLK_RIGHTBRACKET;
    case '\\': return SDLK_BACKSLASH;
    case ';': return SDLK_SEMICOLON;
    case '\'': return SDLK_APOSTROPHE;
    case ',': return SDLK_COMMA;
    case '.': return SDLK_PERIOD;
    case '/': return SDLK_SLASH;
    }
  }

  // Named keys (case-insensitive)
  if (strcasecmp(name, "return") == 0 || strcasecmp(name, "enter") == 0) {
    return SDLK_RETURN;
  }
  if (strcasecmp(name, "escape") == 0 || strcasecmp(name, "esc") == 0) {
    return SDLK_ESCAPE;
  }
  if (strcasecmp(name, "backspace") == 0) {
    return SDLK_BACKSPACE;
  }
  if (strcasecmp(name, "tab") == 0) {
    return SDLK_TAB;
  }
  if (strcasecmp(name, "space") == 0) {
    return SDLK_SPACE;
  }
  if (strcasecmp(name, "delete") == 0 || strcasecmp(name, "del") == 0) {
    return SDLK_DELETE;
  }
  if (strcasecmp(name, "insert") == 0 || strcasecmp(name, "ins") == 0) {
    return SDLK_INSERT;
  }
  if (strcasecmp(name, "home") == 0) {
    return SDLK_HOME;
  }
  if (strcasecmp(name, "end") == 0) {
    return SDLK_END;
  }
  if (strcasecmp(name, "pageup") == 0 || strcasecmp(name, "pgup") == 0) {
    return SDLK_PAGEUP;
  }
  if (strcasecmp(name, "pagedown") == 0 || strcasecmp(name, "pgdn") == 0) {
    return SDLK_PAGEDOWN;
  }
  if (strcasecmp(name, "up") == 0) {
    return SDLK_UP;
  }
  if (strcasecmp(name, "down") == 0) {
    return SDLK_DOWN;
  }
  if (strcasecmp(name, "left") == 0) {
    return SDLK_LEFT;
  }
  if (strcasecmp(name, "right") == 0) {
    return SDLK_RIGHT;
  }
  // Function keys
  if (name[0] == 'f' || name[0] == 'F') {
    int num = atoi(name + 1);
    if (num >= 1 && num <= 12) {
      return SDLK_F1 + (num - 1);
    }
  }

  return 0; // Unknown key
}

// weft::config::keybind key-string script -> register a keybinding
// key-string is like "ctrl+w", "ctrl+shift+s", "f5", etc.
static int c_config_keybind(lcl_interp *interp, int argc, lcl_value **argv,
                            lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (argc < 2) {
    lcl_set_error(interp, "config::keybind requires key-string and script");
    return LCL_RC_ERR;
  }

  const char *key_str = lcl_value_to_string(argv[0]);
  const char *script = lcl_value_to_string(argv[1]);

  if (!key_str || !script) {
    lcl_set_error(interp, "config::keybind: arguments must be strings");
    return LCL_RC_ERR;
  }

  // Parse the key string (e.g., "ctrl+w", "ctrl+shift+s")
  int modifiers = 0;
  int keycode = 0;

  // Make a mutable copy to tokenize
  char *key_copy = strdup(key_str);
  if (!key_copy) {
    lcl_set_error(interp, "out of memory");
    return LCL_RC_ERR;
  }

  // Split by '+' and parse each part
  char *saveptr;
  char *token = strtok_r(key_copy, "+", &saveptr);
  while (token) {
    // Trim whitespace
    while (*token == ' ') {
      token++;
    }
    char *end = token + strlen(token) - 1;
    while (end > token && *end == ' ') {
      *end-- = '\0';
    }

    if (strcasecmp(token, "ctrl") == 0 || strcasecmp(token, "control") == 0) {
      modifiers |= KEYBIND_CTRL;
    } else if (strcasecmp(token, "shift") == 0) {
      modifiers |= KEYBIND_SHIFT;
    } else if (strcasecmp(token, "alt") == 0) {
      modifiers |= KEYBIND_ALT;
    } else {
      // This should be the key itself
      keycode = parse_keycode(token);
      if (keycode == 0) {
        free(key_copy);
        char err[128];
        snprintf(err, sizeof(err), "config::keybind: unknown key '%s'", token);
        lcl_set_error(interp, err);
        return LCL_RC_ERR;
      }
    }
    token = strtok_r(NULL, "+", &saveptr);
  }
  free(key_copy);

  if (keycode == 0) {
    lcl_set_error(interp, "config::keybind: no key specified in key-string");
    return LCL_RC_ERR;
  }

  lcl_host_register_keybind(host, keycode, modifiers, script);

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// weft::ts::highlight ?highlights-path? -> apply tree-sitter highlighting to
// current buffer
static int c_ts_highlight(lcl_interp *interp, int argc, lcl_value **argv,
                          lcl_value **out) {
  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_buffer) {
    lcl_set_error(interp, "no buffer set");
    return LCL_RC_ERR;
  }

  // Get highlights.scm path - use argument or default
  const char *highlights_path = NULL;
  if (argc > 0) {
    highlights_path = lcl_value_to_string(argv[0]);
  }

  // Default to the tree-sitter-lcl queries path
  char default_path[PATH_MAX];
  if (!highlights_path && host->config_dir) {
    // First try config dir
    snprintf(default_path, sizeof(default_path),
             "%s/queries/lcl/highlights.scm", host->config_dir);
    FILE *f = fopen(default_path, "r");
    if (f) {
      fclose(f);
      highlights_path = default_path;
    }
  }

  // Fall back to submodule path (for development)
  if (!highlights_path) {
    // Try relative to common locations
    const char *paths[] = {
        "../submodules/tree-sitter-lcl/queries/highlights.scm",
        "../../submodules/tree-sitter-lcl/queries/highlights.scm",
        "/home/brennan/dev/weft/submodules/tree-sitter-lcl/queries/highlights.scm",
        NULL};
    for (const char **p = paths; *p; p++) {
      FILE *f = fopen(*p, "r");
      if (f) {
        fclose(f);
        highlights_path = *p;
        break;
      }
    }
  }

  if (!highlights_path) {
    lcl_set_error(interp, "ts::highlight: cannot find highlights.scm");
    return LCL_RC_ERR;
  }

  // Get or create highlighter
  TSHighlighter *hl = ts_hl_get_lcl(highlights_path);
  if (!hl) {
    lcl_set_error(interp, "ts::highlight: failed to create highlighter");
    return LCL_RC_ERR;
  }

  // Apply highlighting to the buffer
  ts_hl_apply_to_buffer(hl, host->current_buffer, highlights_path);

  *out = lcl_list_new();
  return LCL_RC_OK;
}

// === Transform Bindings ===

// Helper: Create and display a diff buffer for a patch
static void show_patch_buffer(LclHost *host, Patch *patch, Buffer *target) {
  if (!host || !patch || !target || !host->buffer_manager ||
      !host->layout_root_ptr) {
    return;
  }

  // Generate diff text
  char *diff = patch_to_diff(patch, target->document, 3);
  if (!diff) {
    return;
  }

  // Create a diff buffer
  Buffer *diff_buf = buffer_manager_create(host->buffer_manager);
  if (!diff_buf) {
    free(diff);
    return;
  }

  // Give the diff buffer a name so it appears in buffer list
  diff_buf->file_path = strdup("*diff*");

  // Set buffer content
  document_insert(diff_buf->document, 0, diff, strlen(diff));
  free(diff);

  // Add header with actions
  char header[512];
  size_t additions, deletions, edit_count;
  patch_stats(patch, &additions, &deletions, &edit_count);
  snprintf(header, sizeof(header),
           ";; Patch: %s\n"
           ";; %zu edit(s), +%zu/-%zu bytes\n"
           ";; [weft::transform::apply] [weft::transform::reject]\n"
           ";;\n",
           patch->transform_name ? patch->transform_name : "unnamed",
           edit_count, additions, deletions);

  document_insert(diff_buf->document, 0, header, strlen(header));

  // Create a pane for the diff buffer
  Pane *new_pane = pane_new(diff_buf, host->char_width, host->line_height);
  if (!new_pane) {
    buffer_manager_close(host->buffer_manager, diff_buf);
    return;
  }

  // Apply viewlayer configuration
  lcl_host_apply_viewlayers(host, new_pane);

  // Split current layout horizontally to show new pane
  LayoutNode *root = *host->layout_root_ptr;
  LayoutNode *new_split = layout_split_horizontal(root, new_pane);

  if (new_split && !new_split->parent) {
    *host->layout_root_ptr = new_split;
  }

  // Recompute layout bounds
  recompute_layout_bounds(host);

  // Focus the new pane
  if (host->focused_pane_ptr) {
    *host->focused_pane_ptr = new_pane;
  }
}

// weft::transform::register name description script -> register an Lcl
// transform
static int c_transform_register(lcl_interp *interp, int argc, lcl_value **argv,
                                lcl_value **out) {
  if (argc < 3) {
    lcl_set_error(interp,
                  "transform::register requires name, description, and script");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  const char *name = lcl_value_to_string(argv[0]);
  const char *description = lcl_value_to_string(argv[1]);
  const char *script = lcl_value_to_string(argv[2]);

  if (!name || !description || !script) {
    lcl_set_error(interp, "arguments must be strings");
    return LCL_RC_ERR;
  }

  if (!transform_register_lcl(name, description, host, script)) {
    lcl_set_error(interp, "failed to register transform (name may exist)");
    return LCL_RC_ERR;
  }

  *out = lcl_string_new(name);
  return LCL_RC_OK;
}

// weft::transform::replace text -> create tagged replace result
static int c_transform_replace(lcl_interp *interp, int argc, lcl_value **argv,
                               lcl_value **out) {
  (void)interp;

  if (argc < 1) {
    lcl_set_error(interp, "transform::replace requires text argument");
    return LCL_RC_ERR;
  }

  // Create {replace <text>} list
  lcl_value *result = lcl_list_new();
  lcl_list_push(&result, lcl_string_new("replace"));
  lcl_list_push(&result, lcl_ref_inc(argv[0]));

  *out = result;
  return LCL_RC_OK;
}

// weft::transform::cancel -> create tagged cancel result
static int c_transform_cancel(lcl_interp *interp, int argc, lcl_value **argv,
                              lcl_value **out) {
  (void)interp;
  (void)argc;
  (void)argv;

  // Create {cancel} list
  lcl_value *result = lcl_list_new();
  lcl_list_push(&result, lcl_string_new("cancel"));

  *out = result;
  return LCL_RC_OK;
}

// weft::transform::patch edits -> create tagged patch result
// edits should be a list of {start end text} triples
static int c_transform_patch(lcl_interp *interp, int argc, lcl_value **argv,
                             lcl_value **out) {
  (void)interp;

  if (argc < 1) {
    lcl_set_error(interp, "transform::patch requires edits list argument");
    return LCL_RC_ERR;
  }

  // Create {patch <edits>} list
  lcl_value *result = lcl_list_new();
  lcl_list_push(&result, lcl_string_new("patch"));
  lcl_list_push(&result, lcl_ref_inc(argv[0]));

  *out = result;
  return LCL_RC_OK;
}

// weft::transform::prompt message ?default? -> create tagged prompt result
static int c_transform_prompt(lcl_interp *interp, int argc, lcl_value **argv,
                              lcl_value **out) {
  (void)interp;

  if (argc < 1) {
    lcl_set_error(interp, "transform::prompt requires message argument");
    return LCL_RC_ERR;
  }

  // Create {prompt <message> ?<default>?} list
  lcl_value *result = lcl_list_new();
  lcl_list_push(&result, lcl_string_new("prompt"));
  lcl_list_push(&result, lcl_ref_inc(argv[0]));

  if (argc >= 2) {
    lcl_list_push(&result, lcl_ref_inc(argv[1]));
  }

  *out = result;
  return LCL_RC_OK;
}

// weft::transform::run name ?args...? -> run transform on selection
static int c_transform_run(lcl_interp *interp, int argc, lcl_value **argv,
                           lcl_value **out) {
  if (argc < 1) {
    lcl_set_error(interp, "transform::run requires a transform name");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_buffer || !host->current_editor) {
    lcl_set_error(interp, "no buffer/editor context");
    return LCL_RC_ERR;
  }

  const char *name = lcl_value_to_string(argv[0]);
  if (!name) {
    lcl_set_error(interp, "transform name must be a string");
    return LCL_RC_ERR;
  }

  // Determine target buffer and editor
  // If we're in the REPL with an attached buffer, use the attached buffer
  Buffer *target_buffer = host->current_buffer;
  Editor *target_editor = host->current_editor;

  if (host->repl_attached_buffer) {
    // We have an attached buffer - use it instead
    Pane *attached_pane =
        find_pane_for_buffer(host, host->repl_attached_buffer);
    if (attached_pane && attached_pane->view) {
      target_buffer = host->repl_attached_buffer;
      target_editor = attached_pane->view;
    }
  }

  // Get selection range from target editor
  size_t start, end;
  if (editor_has_selection(target_editor)) {
    editor_get_selection(target_editor, &start, &end);
  } else {
    // No selection - use entire document
    start = 0;
    end = document_length(target_buffer->document);
  }

  // Collect additional args (if any)
  const char **args = NULL;
  if (argc > 1) {
    args = calloc(argc, sizeof(char *));
    if (args) {
      for (int i = 1; i < argc; i++) {
        args[i - 1] = lcl_value_to_string(argv[i]);
      }
      args[argc - 1] = NULL;
    }
  }

  // Run the transform on target buffer
  Patch *patch = transform_run(name, target_buffer, start, end, args);
  free(args);

  if (!patch) {
    // Check if transform requested a prompt
    if (lcl_host_has_transform_prompt(host)) {
      *out = lcl_string_new("prompt requested");
      return LCL_RC_OK;
    }
    lcl_set_error(interp, "transform failed or not found");
    return LCL_RC_ERR;
  }

  // Store pending patch
  if (host->pending_patch) {
    patch_free(host->pending_patch);
  }
  host->pending_patch = patch;
  host->patch_target_buffer = target_buffer;

  // Show the diff buffer
  show_patch_buffer(host, patch, target_buffer);

  *out = lcl_string_new("patch created");
  return LCL_RC_OK;
}

// weft::transform::apply -> apply the pending patch
static int c_transform_apply(lcl_interp *interp, int argc, lcl_value **argv,
                             lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (!host->pending_patch || !host->patch_target_buffer) {
    lcl_set_error(interp, "no pending patch to apply");
    return LCL_RC_ERR;
  }

  // Apply the patch
  Buffer *target = host->patch_target_buffer;
  PatchResult result = patch_apply(host->pending_patch, target);

  // Clear selection in the target buffer's editor (old selection is now stale)
  Pane *target_pane = find_pane_for_buffer(host, target);
  if (target_pane && target_pane->view) {
    editor_clear_selection(target_pane->view);
  }

  // Clean up
  patch_free(host->pending_patch);
  host->pending_patch = NULL;
  host->patch_target_buffer = NULL;

  if (result != PATCH_OK) {
    lcl_set_error(interp, "failed to apply patch");
    return LCL_RC_ERR;
  }

  // Kill the diff buffer (destroys *diff* buffer)
  kill_focused_pane(host);

  *out = lcl_string_new("patch applied");
  return LCL_RC_OK;
}

// weft::transform::reject -> reject the pending patch
static int c_transform_reject(lcl_interp *interp, int argc, lcl_value **argv,
                              lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  if (!host->pending_patch) {
    *out = lcl_string_new("no pending patch");
    return LCL_RC_OK;
  }

  // Clean up
  patch_free(host->pending_patch);
  host->pending_patch = NULL;
  host->patch_target_buffer = NULL;

  // Kill the diff buffer (destroys *diff* buffer)
  kill_focused_pane(host);

  *out = lcl_string_new("patch rejected");
  return LCL_RC_OK;
}

// weft::transform::apply-ctx ctx new-text -> apply transformation to context
// ctx is a dict with keys: start, end, buffer, text
// Creates a patch and shows diff preview
static int c_transform_apply_ctx(lcl_interp *interp, int argc, lcl_value **argv,
                                 lcl_value **out) {
  if (argc < 2) {
    lcl_set_error(interp,
                  "weft::transform::apply-ctx requires context and new-text");
    return LCL_RC_ERR;
  }

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->buffer_manager) {
    lcl_set_error(interp, "no host context");
    return LCL_RC_ERR;
  }

  // Parse context dict
  lcl_value *ctx = argv[0];
  if (lcl_value_type_of(ctx) != LCL_DICT) {
    lcl_set_error(interp, "context must be a dict");
    return LCL_RC_ERR;
  }

  // Extract fields from context: #{start N end N buffer N text "..."}
  long start = -1, end = -1, buffer_id = -1;
  const char *old_text = NULL;

  lcl_value *val = NULL;
  if (lcl_dict_get(ctx, "start", &val) == LCL_OK) {
    lcl_value_to_int(val, &start);
  }
  if (lcl_dict_get(ctx, "end", &val) == LCL_OK) {
    lcl_value_to_int(val, &end);
  }
  if (lcl_dict_get(ctx, "buffer", &val) == LCL_OK) {
    lcl_value_to_int(val, &buffer_id);
  }
  if (lcl_dict_get(ctx, "text", &val) == LCL_OK) {
    old_text = lcl_value_to_string(val);
  }

  if (start < 0 || end < 0 || buffer_id < 0) {
    lcl_set_error(interp, "context must have start, end, and buffer fields");
    return LCL_RC_ERR;
  }

  // Get the new text
  const char *new_text = lcl_value_to_string(argv[1]);
  if (!new_text) {
    lcl_set_error(interp, "new-text must be a string");
    return LCL_RC_ERR;
  }

  // Find the target buffer
  Buffer *target_buffer =
      buffer_manager_find_by_id(host->buffer_manager, (uint32_t)buffer_id);
  if (!target_buffer) {
    lcl_set_error(interp, "buffer not found");
    return LCL_RC_ERR;
  }

  // If old text equals new text, nothing to do
  if (old_text && strcmp(old_text, new_text) == 0) {
    *out = lcl_string_new("no changes");
    return LCL_RC_OK;
  }

  // Create a patch
  Patch *patch = patch_new("transform");
  if (!patch) {
    lcl_set_error(interp, "failed to create patch");
    return LCL_RC_ERR;
  }

  // Add the edit
  patch_add(patch, (size_t)start, (size_t)end, new_text);
  patch_set_target(patch, buffer_get_doc_rev(target_buffer),
                   document_length(target_buffer->document));

  // Store pending patch
  if (host->pending_patch) {
    patch_free(host->pending_patch);
  }
  host->pending_patch = patch;
  host->patch_target_buffer = target_buffer;

  // Show the diff buffer
  show_patch_buffer(host, patch, target_buffer);

  *out = lcl_string_new("patch created");
  return LCL_RC_OK;
}

// weft::transform::list -> list available transforms
static int c_transform_list(lcl_interp *interp, int argc, lcl_value **argv,
                            lcl_value **out) {
  (void)argc;
  (void)argv;
  (void)interp;

  // Get list of transforms
  const char *names[64];
  size_t count = transform_list(names, 64);

  lcl_value *result = lcl_list_new();
  for (size_t i = 0; i < count; i++) {
    lcl_list_push(&result, lcl_string_new(names[i]));
  }

  *out = result;
  return LCL_RC_OK;
}

// weft::transform::menu -> insert transform menu at cursor as executable text
static int c_transform_menu(lcl_interp *interp, int argc, lcl_value **argv,
                            lcl_value **out) {
  (void)argc;
  (void)argv;

  LclHost *host = lcl_interp_get_user_data(interp);
  if (!host || !host->current_buffer || !host->current_editor) {
    lcl_set_error(interp, "no buffer/editor context");
    return LCL_RC_ERR;
  }

  // Get list of transforms
  const char *names[64];
  size_t count = transform_list(names, 64);

  if (count == 0) {
    *out = lcl_string_new("No transforms available");
    return LCL_RC_OK;
  }

  // Build menu text
  // Format: weft::transform::run "name"   ;; description
  char menu[4096];
  size_t pos = 0;

  pos += snprintf(menu + pos, sizeof(menu) - pos,
                  ";; Transforms (select line and Ctrl+Enter to run)\n");

  for (size_t i = 0; i < count && pos < sizeof(menu) - 256; i++) {
    const char *desc = transform_get_description(names[i]);
    pos += snprintf(menu + pos, sizeof(menu) - pos,
                    "weft::transform::run \"%s\"", names[i]);
    if (desc && desc[0]) {
      pos += snprintf(menu + pos, sizeof(menu) - pos, "   ;; %s", desc);
    }
    pos += snprintf(menu + pos, sizeof(menu) - pos, "\n");
  }

  // Insert at cursor
  size_t cursor = editor_get_cursor_pos(host->current_editor);
  document_insert(host->current_buffer->document, cursor, menu, strlen(menu));
  editor_set_cursor_pos(host->current_editor, cursor + strlen(menu));

  *out = lcl_int_new((long)count);
  return LCL_RC_OK;
}

// === Internal: Register Weft Bindings ===

static void register_weft_bindings(lcl_interp *interp) {
  // Create weft namespace
  lcl_value *weft_ns = lcl_ns_new("weft");
  lcl_define_take(interp, "weft", weft_ns);

  lcl_ns_def(weft_ns, "version",
             lcl_c_proc_new("weft::version", c_weft_version));

  // Create weft::doc namespace for document operations
  lcl_value *doc_ns = lcl_ns_new("doc");
  lcl_ns_def(weft_ns, "doc", doc_ns);

  lcl_ns_def(doc_ns, "length",
             lcl_c_proc_new("weft::doc::length", c_doc_length));
  lcl_ns_def(doc_ns, "line-count",
             lcl_c_proc_new("weft::doc::line-count", c_doc_line_count));
  lcl_ns_def(doc_ns, "text", lcl_c_proc_new("weft::doc::text", c_doc_text));
  lcl_ns_def(doc_ns, "line-start",
             lcl_c_proc_new("weft::doc::line-start", c_doc_line_start));
  lcl_ns_def(doc_ns, "line-end",
             lcl_c_proc_new("weft::doc::line-end", c_doc_line_end));

  // Create weft::annot namespace for annotation operations
  lcl_value *annot_ns = lcl_ns_new("annot");
  lcl_ns_def(weft_ns, "annot", annot_ns);

  lcl_ns_def(annot_ns, "add", lcl_c_proc_new("weft::annot::add", c_annot_add));
  lcl_ns_def(annot_ns, "remove",
             lcl_c_proc_new("weft::annot::remove", c_annot_remove));
  lcl_ns_def(annot_ns, "at", lcl_c_proc_new("weft::annot::at", c_annot_at));
  lcl_ns_def(annot_ns, "in-range",
             lcl_c_proc_new("weft::annot::in-range", c_annot_in_range));
  lcl_ns_def(annot_ns, "get", lcl_c_proc_new("weft::annot::get", c_annot_get));
  lcl_ns_def(annot_ns, "clear-layer",
             lcl_c_proc_new("weft::annot::clear-layer", c_annot_clear_layer));
  lcl_ns_def(annot_ns, "edit-at",
             lcl_c_proc_new("weft::annot::edit-at", c_annot_edit_at));
  lcl_ns_def(annot_ns, "edit-range",
             lcl_c_proc_new("weft::annot::edit-range", c_annot_edit_range));
  lcl_ns_def(annot_ns, "apply-edit",
             lcl_c_proc_new("weft::annot::apply-edit", c_annot_apply_edit));
  lcl_ns_def(annot_ns, "cancel-edit",
             lcl_c_proc_new("weft::annot::cancel-edit", c_annot_cancel_edit));
  lcl_ns_def(annot_ns, "persist",
             lcl_c_proc_new("weft::annot::persist", c_annot_persist));
  lcl_ns_def(annot_ns, "ephemeral",
             lcl_c_proc_new("weft::annot::ephemeral", c_annot_ephemeral));
  lcl_ns_def(annot_ns, "orphans",
             lcl_c_proc_new("weft::annot::orphans", c_annot_orphans));
  lcl_ns_def(
      annot_ns, "resolve-orphan",
      lcl_c_proc_new("weft::annot::resolve-orphan", c_annot_resolve_orphan));
  lcl_ns_def(
      annot_ns, "discard-orphan",
      lcl_c_proc_new("weft::annot::discard-orphan", c_annot_discard_orphan));

  // Create weft::interact namespace for interaction event context
  lcl_value *interact_ns = lcl_ns_new("interact");
  lcl_ns_def(weft_ns, "interact", interact_ns);

  lcl_ns_def(
      interact_ns, "event-type",
      lcl_c_proc_new("weft::interact::event-type", c_interact_event_type));
  lcl_ns_def(interact_ns, "position",
             lcl_c_proc_new("weft::interact::position", c_interact_position));
  lcl_ns_def(interact_ns, "selection",
             lcl_c_proc_new("weft::interact::selection", c_interact_selection));
  lcl_ns_def(
      interact_ns, "word-at-pos",
      lcl_c_proc_new("weft::interact::word-at-pos", c_interact_word_at_pos));
  lcl_ns_def(interact_ns, "modifiers",
             lcl_c_proc_new("weft::interact::modifiers", c_interact_modifiers));
  lcl_ns_def(interact_ns, "handled",
             lcl_c_proc_new("weft::interact::handled", c_interact_handled));

  // Create weft::editor namespace for editor operations
  lcl_value *editor_ns = lcl_ns_new("editor");
  lcl_ns_def(weft_ns, "editor", editor_ns);

  lcl_ns_def(editor_ns, "cursor",
             lcl_c_proc_new("weft::editor::cursor", c_editor_cursor));
  lcl_ns_def(editor_ns, "cursor-set",
             lcl_c_proc_new("weft::editor::cursor-set", c_editor_cursor_set));
  lcl_ns_def(editor_ns, "insert",
             lcl_c_proc_new("weft::editor::insert", c_editor_insert));
  lcl_ns_def(editor_ns, "delete",
             lcl_c_proc_new("weft::editor::delete", c_editor_delete));

  // Create weft::shell namespace for shell command execution
  lcl_value *shell_ns = lcl_ns_new("shell");
  lcl_ns_def(weft_ns, "shell", shell_ns);

  lcl_ns_def(shell_ns, "exec",
             lcl_c_proc_new("weft::shell::exec", c_shell_exec));
  lcl_ns_def(
      shell_ns, "pipe-selection",
      lcl_c_proc_new("weft::shell::pipe-selection", c_shell_pipe_selection));

  // Create weft::buffer namespace for buffer operations
  lcl_value *buffer_ns = lcl_ns_new("buffer");
  lcl_ns_def(weft_ns, "buffer", buffer_ns);

  lcl_ns_def(buffer_ns, "id", lcl_c_proc_new("weft::buffer::id", c_buffer_id));
  lcl_ns_def(buffer_ns, "by-id",
             lcl_c_proc_new("weft::buffer::by-id", c_buffer_by_id));
  lcl_ns_def(buffer_ns, "list",
             lcl_c_proc_new("weft::buffer::list", c_buffer_list));
  lcl_ns_def(buffer_ns, "open",
             lcl_c_proc_new("weft::buffer::open", c_buffer_open));
  lcl_ns_def(buffer_ns, "open-output",
             lcl_c_proc_new("weft::buffer::open-output", c_buffer_open_output));
  lcl_ns_def(buffer_ns, "open-at",
             lcl_c_proc_new("weft::buffer::open-at", c_buffer_open_at));
  lcl_ns_def(buffer_ns, "menu",
             lcl_c_proc_new("weft::buffer::menu", c_buffer_menu));
  lcl_ns_def(buffer_ns, "close",
             lcl_c_proc_new("weft::buffer::close", c_buffer_close));
  lcl_ns_def(buffer_ns, "save",
             lcl_c_proc_new("weft::buffer::save", c_buffer_save));
  lcl_ns_def(buffer_ns, "show",
             lcl_c_proc_new("weft::buffer::show", c_buffer_show));
  lcl_ns_def(buffer_ns, "kill",
             lcl_c_proc_new("weft::buffer::kill", c_buffer_kill));
  lcl_ns_def(buffer_ns, "new",
             lcl_c_proc_new("weft::buffer::new", c_buffer_new));

  // Create weft::pane namespace for pane operations
  lcl_value *pane_ns = lcl_ns_new("pane");
  lcl_ns_def(weft_ns, "pane", pane_ns);

  lcl_ns_def(pane_ns, "close",
             lcl_c_proc_new("weft::pane::close", c_pane_close));
  lcl_ns_def(pane_ns, "next", lcl_c_proc_new("weft::pane::next", c_pane_next));
  lcl_ns_def(pane_ns, "prev", lcl_c_proc_new("weft::pane::prev", c_pane_prev));

  // Create weft::viewlayer namespace for dynamic layer management
  lcl_value *viewlayer_ns = lcl_ns_new("viewlayer");
  lcl_ns_def(weft_ns, "viewlayer", viewlayer_ns);

  lcl_ns_def(viewlayer_ns, "add",
             lcl_c_proc_new("weft::viewlayer::add", c_viewlayer_add));

  // Create weft::fs namespace for filesystem operations
  lcl_value *fs_ns = lcl_ns_new("fs");
  lcl_ns_def(weft_ns, "fs", fs_ns);

  lcl_ns_def(fs_ns, "ls", lcl_c_proc_new("weft::fs::ls", c_fs_ls));

  // Create weft::plumb namespace for plumber operations
  lcl_value *plumb_ns = lcl_ns_new("plumb");
  lcl_ns_def(weft_ns, "plumb", plumb_ns);

  lcl_ns_def(plumb_ns, "scan",
             lcl_c_proc_new("weft::plumb::scan", c_plumb_scan));

  // Create weft::repl namespace for REPL operations
  lcl_value *repl_ns = lcl_ns_new("repl");
  lcl_ns_def(weft_ns, "repl", repl_ns);

  lcl_ns_def(repl_ns, "open", lcl_c_proc_new("weft::repl::open", c_repl_open));
  lcl_ns_def(repl_ns, "attach",
             lcl_c_proc_new("weft::repl::attach", c_repl_attach));
  lcl_ns_def(repl_ns, "detach",
             lcl_c_proc_new("weft::repl::detach", c_repl_detach));
  lcl_ns_def(repl_ns, "target",
             lcl_c_proc_new("weft::repl::target", c_repl_target));
  lcl_ns_def(
      repl_ns, "target-selection",
      lcl_c_proc_new("weft::repl::target-selection", c_repl_target_selection));
  lcl_ns_def(repl_ns, "target-cursor",
             lcl_c_proc_new("weft::repl::target-cursor", c_repl_target_cursor));
  lcl_ns_def(repl_ns, "goto", lcl_c_proc_new("weft::repl::goto", c_repl_goto));

  // Create weft::sel namespace for selection access
  lcl_value *sel_ns = lcl_ns_new("sel");
  lcl_ns_def(weft_ns, "sel", sel_ns);

  lcl_ns_def(sel_ns, "start", lcl_c_proc_new("weft::sel::start", c_sel_start));
  lcl_ns_def(sel_ns, "end", lcl_c_proc_new("weft::sel::end", c_sel_end));
  lcl_ns_def(sel_ns, "text", lcl_c_proc_new("weft::sel::text", c_sel_text));
  lcl_ns_def(sel_ns, "buffer",
             lcl_c_proc_new("weft::sel::buffer", c_sel_buffer));
  lcl_ns_def(sel_ns, "exists?",
             lcl_c_proc_new("weft::sel::exists?", c_sel_exists));
  lcl_ns_def(sel_ns, "info", lcl_c_proc_new("weft::sel::info", c_sel_info));

  // Create weft::inspect namespace for annotation inspection
  lcl_value *inspect_ns = lcl_ns_new("inspect");
  lcl_ns_def(weft_ns, "inspect", inspect_ns);

  lcl_ns_def(inspect_ns, "at",
             lcl_c_proc_new("weft::inspect::at", c_inspect_at));

  // Create weft::config namespace for configuration
  lcl_value *config_ns = lcl_ns_new("config");
  lcl_ns_def(weft_ns, "config", config_ns);

  lcl_ns_def(config_ns, "filetype",
             lcl_c_proc_new("weft::config::filetype", c_config_filetype));
  lcl_ns_def(config_ns, "handler",
             lcl_c_proc_new("weft::config::handler", c_config_handler));
  lcl_ns_def(config_ns, "dir",
             lcl_c_proc_new("weft::config::dir", c_config_dir));
  lcl_ns_def(config_ns, "indent",
             lcl_c_proc_new("weft::config::indent", c_config_indent));
  lcl_ns_def(config_ns, "wrap",
             lcl_c_proc_new("weft::config::wrap", c_config_wrap));
  lcl_ns_def(config_ns, "font",
             lcl_c_proc_new("weft::config::font", c_config_font));
  lcl_ns_def(config_ns, "keybind",
             lcl_c_proc_new("weft::config::keybind", c_config_keybind));
  lcl_ns_def(config_ns, "viewlayer",
             lcl_c_proc_new("weft::config::viewlayer", c_config_viewlayer));
  lcl_ns_def(config_ns, "language",
             lcl_c_proc_new("weft::config::language", c_config_language));
  lcl_ns_def(
      config_ns, "language-ext",
      lcl_c_proc_new("weft::config::language-ext", c_config_language_ext));

  // Create weft::ts namespace for tree-sitter operations
  lcl_value *ts_ns = lcl_ns_new("ts");
  lcl_ns_def(weft_ns, "ts", ts_ns);

  lcl_ns_def(ts_ns, "highlight",
             lcl_c_proc_new("weft::ts::highlight", c_ts_highlight));

  // Create weft::transform namespace for transform operations
  lcl_value *transform_ns = lcl_ns_new("transform");
  lcl_ns_def(weft_ns, "transform", transform_ns);

  lcl_ns_def(transform_ns, "run",
             lcl_c_proc_new("weft::transform::run", c_transform_run));
  lcl_ns_def(transform_ns, "apply",
             lcl_c_proc_new("weft::transform::apply", c_transform_apply));
  lcl_ns_def(transform_ns, "reject",
             lcl_c_proc_new("weft::transform::reject", c_transform_reject));
  lcl_ns_def(
      transform_ns, "apply-ctx",
      lcl_c_proc_new("weft::transform::apply-ctx", c_transform_apply_ctx));
  lcl_ns_def(transform_ns, "list",
             lcl_c_proc_new("weft::transform::list", c_transform_list));
  lcl_ns_def(transform_ns, "menu",
             lcl_c_proc_new("weft::transform::menu", c_transform_menu));
  lcl_ns_def(transform_ns, "register",
             lcl_c_proc_new("weft::transform::register", c_transform_register));

  // Tagged return helpers for Lcl transforms
  lcl_ns_def(transform_ns, "replace",
             lcl_c_proc_new("weft::transform::replace", c_transform_replace));
  lcl_ns_def(transform_ns, "cancel",
             lcl_c_proc_new("weft::transform::cancel", c_transform_cancel));
  lcl_ns_def(transform_ns, "patch",
             lcl_c_proc_new("weft::transform::patch", c_transform_patch));
  lcl_ns_def(transform_ns, "prompt",
             lcl_c_proc_new("weft::transform::prompt", c_transform_prompt));
}

// === Public API ===

LclHost *lcl_host_new(void) {
  LclHost *host = calloc(1, sizeof(LclHost));
  if (!host) {
    return NULL;
  }

  host->interp = lcl_interp_new();
  if (!host->interp) {
    free(host);
    return NULL;
  }

  // Register optional Lcl packages
  lcl_register_regex(host->interp);

  // Initialize registered layers array
  host->registered_layers = calloc(INITIAL_LAYER_CAP, sizeof(RegisteredLayer));
  if (!host->registered_layers) {
    lcl_interp_free(host->interp);
    free(host);
    return NULL;
  }
  host->registered_layer_cap = INITIAL_LAYER_CAP;
  host->registered_layer_count = 0;

  // Initialize registered handlers array
  host->handlers = calloc(INITIAL_HANDLER_CAP, sizeof(RegisteredHandler));
  if (!host->handlers) {
    free(host->registered_layers);
    lcl_interp_free(host->interp);
    free(host);
    return NULL;
  }
  host->handler_cap = INITIAL_HANDLER_CAP;
  host->handler_count = 0;

  // Initialize file type associations array
  host->filetypes = calloc(INITIAL_LAYER_CAP, sizeof(FileTypeAssoc));
  if (!host->filetypes) {
    free(host->handlers);
    free(host->registered_layers);
    lcl_interp_free(host->interp);
    free(host);
    return NULL;
  }
  host->filetype_cap = INITIAL_LAYER_CAP;
  host->filetype_count = 0;

  // Initialize indent settings array
  host->indent_settings = calloc(INITIAL_LAYER_CAP, sizeof(IndentSettings));
  if (!host->indent_settings) {
    free(host->filetypes);
    free(host->handlers);
    free(host->registered_layers);
    lcl_interp_free(host->interp);
    free(host);
    return NULL;
  }
  host->indent_cap = INITIAL_LAYER_CAP;
  host->indent_count = 0;

  // Initialize wrap settings array
  host->wrap_settings = calloc(INITIAL_LAYER_CAP, sizeof(WrapSettings));
  if (!host->wrap_settings) {
    free(host->indent_settings);
    free(host->filetypes);
    free(host->handlers);
    free(host->registered_layers);
    lcl_interp_free(host->interp);
    free(host);
    return NULL;
  }
  host->wrap_cap = INITIAL_LAYER_CAP;
  host->wrap_count = 0;

  // Initialize font config to default values
  host->font_config.path = NULL;
  host->font_config.size = 14; // Default size
  host->font_config_set = false;

  // Initialize keybindings array
  host->keybindings = calloc(INITIAL_HANDLER_CAP, sizeof(RegisteredKeybind));
  if (!host->keybindings) {
    free(host->wrap_settings);
    free(host->indent_settings);
    free(host->filetypes);
    free(host->handlers);
    free(host->registered_layers);
    lcl_interp_free(host->interp);
    free(host);
    return NULL;
  }
  host->keybind_cap = INITIAL_HANDLER_CAP;
  host->keybind_count = 0;

  // Initialize viewlayer configs array
  host->viewlayer_configs = calloc(INITIAL_LAYER_CAP, sizeof(ViewLayerConfig));
  if (!host->viewlayer_configs) {
    free(host->keybindings);
    free(host->wrap_settings);
    free(host->indent_settings);
    free(host->filetypes);
    free(host->handlers);
    free(host->registered_layers);
    lcl_interp_free(host->interp);
    free(host);
    return NULL;
  }
  host->viewlayer_cap = INITIAL_LAYER_CAP;
  host->viewlayer_count = 0;

  // Set up config directory path (~/.config/weft)
  const char *home = getenv("HOME");
  if (home) {
    size_t len = strlen(home) + strlen("/.config/weft") + 1;
    host->config_dir = malloc(len);
    if (host->config_dir) {
      snprintf(host->config_dir, len, "%s/.config/weft", home);
    }
  }

  // Set up data directory for annotation persistence (~/.local/share/weft)
  host->data_dir = get_xdg_data_dir("weft");
  if (host->data_dir) {
    mkdir_recursive(host->data_dir);
    // Create annots subdirectory
    char *annots_dir = path_join(host->data_dir, "annots");
    if (annots_dir) {
      mkdir_recursive(annots_dir);
      free(annots_dir);
    }

    // Initialize document registry
    host->doc_registry = doc_registry_new(host->data_dir);
    if (host->doc_registry) {
      doc_registry_load(host->doc_registry);
    }
  }

  // Initialize persistent layers array
  host->persistent_layers = NULL;
  host->persistent_layer_count = 0;
  host->persistent_layer_cap = 0;

  // Initialize tree-sitter language registry
  host->ts_registry = ts_lang_registry_new();
  if (!host->ts_registry) {
    free(host->config_dir);
    free(host->viewlayer_configs);
    free(host->keybindings);
    free(host->wrap_settings);
    free(host->indent_settings);
    free(host->filetypes);
    free(host->handlers);
    free(host->registered_layers);
    lcl_interp_free(host->interp);
    free(host);
    return NULL;
  }

  // Register embedded tree-sitter parsers
  // These are conditionally compiled based on WEFT_EMBED_TS_* defines
  // WEFT_SUBMODULES_DIR is defined by CMake as an absolute path
#ifdef WEFT_EMBED_TS_LCL
  ts_lang_registry_add_embedded(host->ts_registry, "lcl", tree_sitter_lcl, NULL,
                                WEFT_SUBMODULES_DIR
                                "/tree-sitter-lcl/queries/highlights.scm");
  ts_lang_registry_map_ext(host->ts_registry, ".lcl", "lcl");
#endif

#ifdef WEFT_EMBED_TS_C
  ts_lang_registry_add_embedded(host->ts_registry, "c", tree_sitter_c, NULL,
                                WEFT_SUBMODULES_DIR
                                "/c/queries/highlights.scm");
  ts_lang_registry_map_ext(host->ts_registry, ".c", "c");
  ts_lang_registry_map_ext(host->ts_registry, ".h", "c");
#endif

#ifdef WEFT_EMBED_TS_JSON
  ts_lang_registry_add_embedded(
      host->ts_registry, "json", tree_sitter_json, NULL,
      WEFT_SUBMODULES_DIR "/tree-sitter-json/queries/highlights.scm");
  ts_lang_registry_map_ext(host->ts_registry, ".json", "json");
#endif

#ifdef WEFT_EMBED_TS_BASH
  ts_lang_registry_add_embedded(
      host->ts_registry, "bash", tree_sitter_bash, NULL,
      WEFT_SUBMODULES_DIR "/tree-sitter-bash/queries/highlights.scm");
  ts_lang_registry_map_ext(host->ts_registry, ".sh", "bash");
  ts_lang_registry_map_ext(host->ts_registry, ".bash", "bash");
#endif

#ifdef WEFT_EMBED_TS_JAVASCRIPT
  ts_lang_registry_add_embedded(
      host->ts_registry, "javascript", tree_sitter_javascript, NULL,
      WEFT_SUBMODULES_DIR "/tree-sitter-javascript/queries/highlights.scm");
  ts_lang_registry_map_ext(host->ts_registry, ".js", "javascript");
  ts_lang_registry_map_ext(host->ts_registry, ".mjs", "javascript");
  ts_lang_registry_map_ext(host->ts_registry, ".jsx", "javascript");
#endif

  // Register core Lcl commands
  lcl_register_core(host->interp);

  // Set user data so bindings can access the host
  lcl_interp_set_user_data(host->interp, host);

  // Register Weft-specific bindings
  register_weft_bindings(host->interp);

  // Load embedded stdlib (defines common aliases like upper, lower, etc.)
#ifdef EMBEDDED_STDLIB_AVAILABLE
  {
    lcl_value *result = NULL;
    int rc = lcl_eval_string(host->interp, embedded_stdlib, &result);
    if (result) {
      lcl_ref_dec(result);
    }
    if (rc != LCL_RC_OK && rc != LCL_RC_RETURN) {
      const char *error_msg = lcl_interp_error_msg(host->interp);
      fprintf(stderr, "Warning: Failed to load embedded stdlib: %s\n",
              error_msg ? error_msg : "unknown error");
    }
  }
#endif

  // Initialize transform registry and built-in transforms
  transform_registry_init();
  transform_register_builtins();

  return host;
}

void lcl_host_free(LclHost *host) {
  if (!host) {
    return;
  }

  // Free registered layers
  for (size_t i = 0; i < host->registered_layer_count; i++) {
    free(host->registered_layers[i].name);
    free(host->registered_layers[i].script_path);
  }
  free(host->registered_layers);

  // Free registered handlers
  for (size_t i = 0; i < host->handler_count; i++) {
    free(host->handlers[i].name);
    free(host->handlers[i].script);
  }
  free(host->handlers);

  // Free file type associations
  for (size_t i = 0; i < host->filetype_count; i++) {
    free(host->filetypes[i].extension);
    free(host->filetypes[i].layer_name);
    free(host->filetypes[i].script_path);
  }
  free(host->filetypes);

  // Free indent settings
  for (size_t i = 0; i < host->indent_count; i++) {
    free(host->indent_settings[i].extension);
  }
  free(host->indent_settings);

  // Free wrap settings
  for (size_t i = 0; i < host->wrap_count; i++) {
    free(host->wrap_settings[i].extension);
  }
  free(host->wrap_settings);

  // Free font config
  free(host->font_config.path);

  // Free keybindings
  for (size_t i = 0; i < host->keybind_count; i++) {
    free(host->keybindings[i].script);
  }
  free(host->keybindings);

  // Free viewlayer configs
  for (size_t i = 0; i < host->viewlayer_count; i++) {
    free(host->viewlayer_configs[i].extension);
    free(host->viewlayer_configs[i].param);
  }
  free(host->viewlayer_configs);

  free(host->config_dir);

  // Save and free document registry
  if (host->doc_registry) {
    doc_registry_save(host->doc_registry);
    doc_registry_free(host->doc_registry);
  }
  free(host->data_dir);

  // Free persistent layers
  for (size_t i = 0; i < host->persistent_layer_count; i++) {
    free(host->persistent_layers[i]);
  }
  free(host->persistent_layers);

  // Free tree-sitter language registry
  if (host->ts_registry) {
    ts_lang_registry_free(host->ts_registry);
  }

  // Cleanup tree-sitter highlighters
  ts_hl_cleanup();

  // Free pending patch
  if (host->pending_patch) {
    patch_free(host->pending_patch);
  }

  // Free annotation editor state
  free(host->annot_edit.original_ids);

  // Cleanup transform registry
  transform_registry_cleanup();

  if (host->interp) {
    lcl_interp_free(host->interp);
  }

  free(host);
}

bool lcl_host_eval(LclHost *host, const char *code, const char **error_msg) {
  if (!host || !host->interp || !code) {
    if (error_msg) {
      *error_msg = "invalid arguments";
    }
    return false;
  }

  lcl_value *result = NULL;
  int rc = lcl_eval_string(host->interp, code, &result);

  if (result) {
    lcl_ref_dec(result);
  }

  // Treat LCL_RC_RETURN as success (scripts can use 'return' to exit early)
  if (rc != LCL_RC_OK && rc != LCL_RC_RETURN) {
    if (error_msg) {
      *error_msg = lcl_interp_error_msg(host->interp);
    }
    return false;
  }

  return true;
}

bool lcl_host_eval_file(LclHost *host, const char *path,
                        const char **error_msg) {
  if (!host || !host->interp || !path) {
    if (error_msg) {
      *error_msg = "invalid arguments";
    }
    return false;
  }

  lcl_value *result = NULL;
  int rc = lcl_eval_file(host->interp, path, &result);

  if (result) {
    lcl_ref_dec(result);
  }

  // Treat LCL_RC_RETURN as success (scripts can use 'return' to exit early)
  if (rc != LCL_RC_OK && rc != LCL_RC_RETURN) {
    if (error_msg) {
      *error_msg = lcl_interp_error_msg(host->interp);
    }
    return false;
  }

  return true;
}

char *lcl_host_eval_to_string(LclHost *host, const char *code,
                              const char **error_msg) {
  if (!host || !host->interp || !code) {
    if (error_msg) {
      *error_msg = "invalid arguments";
    }
    return NULL;
  }

  lcl_value *result = NULL;
  int rc = lcl_eval_string(host->interp, code, &result);

  // Treat LCL_RC_RETURN as success (scripts can use 'return' to exit early)
  if (rc != LCL_RC_OK && rc != LCL_RC_RETURN) {
    if (error_msg) {
      *error_msg = lcl_interp_error_msg(host->interp);
    }
    if (result) {
      lcl_ref_dec(result);
    }
    return NULL;
  }

  // Convert result to string
  char *result_str = NULL;
  if (result) {
    const char *str = lcl_value_to_string(result);
    if (str) {
      result_str = strdup(str);
    } else {
      // Empty result
      result_str = strdup("");
    }
    lcl_ref_dec(result);
  } else {
    result_str = strdup("");
  }

  return result_str;
}

void lcl_host_set_buffer(LclHost *host, Buffer *buffer) {
  if (host) {
    host->current_buffer = buffer;
  }
}

void lcl_host_set_editor(LclHost *host, Editor *editor) {
  if (host) {
    host->current_editor = editor;
  }
}

Buffer *lcl_host_get_buffer(LclHost *host) {
  if (!host) {
    return NULL;
  }
  return host->current_buffer;
}

bool lcl_host_run_layer(LclHost *host, const char *layer_name,
                        const char *script, const char **error_msg) {
  if (!host || !host->interp || !layer_name || !script) {
    if (error_msg) {
      *error_msg = "invalid arguments";
    }
    return false;
  }

  if (!host->current_buffer) {
    if (error_msg) {
      *error_msg = "no buffer set";
    }
    return false;
  }

  // Clear previous annotations from this layer
  annot_store_clear_layer(host->current_buffer->annots, layer_name);

  // Execute the layer script
  lcl_value *result = NULL;
  int rc = lcl_eval_string(host->interp, script, &result);

  if (result) {
    lcl_ref_dec(result);
  }

  if (rc != LCL_RC_OK) {
    if (error_msg) {
      *error_msg = lcl_interp_error_msg(host->interp);
    }
    return false;
  }

  return true;
}

bool lcl_host_run_layer_file(LclHost *host, const char *layer_name,
                             const char *path, const char **error_msg) {
  if (!host || !host->interp || !layer_name || !path) {
    if (error_msg) {
      *error_msg = "invalid arguments";
    }
    return false;
  }

  if (!host->current_buffer) {
    if (error_msg) {
      *error_msg = "no buffer set";
    }
    return false;
  }

  // Clear previous annotations from this layer
  annot_store_clear_layer(host->current_buffer->annots, layer_name);

  // Set current script path for provenance tracking
  char *prev_script_path = host->current_script_path;
  host->current_script_path = strdup(path);

  // Execute the layer script file
  lcl_value *result = NULL;
  int rc = lcl_eval_file(host->interp, path, &result);

  // Restore previous script path
  free(host->current_script_path);
  host->current_script_path = prev_script_path;

  if (result) {
    lcl_ref_dec(result);
  }

  if (rc != LCL_RC_OK) {
    if (error_msg) {
      *error_msg = lcl_interp_error_msg(host->interp);
    }
    return false;
  }

  // Mark layer as valid after successful execution
  annot_store_set_layer_valid(host->current_buffer->annots, layer_name);

  return true;
}

// === Layer Registration ===

// Helper to duplicate a string
static char *lcl_strdup(const char *s) {
  if (!s) {
    return NULL;
  }
  size_t len = strlen(s) + 1;
  char *dup = malloc(len);
  if (dup) {
    memcpy(dup, s, len);
  }
  return dup;
}

// Find registered layer by name (returns NULL if not found)
static RegisteredLayer *find_registered_layer(LclHost *host, const char *name) {
  for (size_t i = 0; i < host->registered_layer_count; i++) {
    if (host->registered_layers[i].name &&
        strcmp(host->registered_layers[i].name, name) == 0) {
      return &host->registered_layers[i];
    }
  }
  return NULL;
}

void lcl_host_register_layer(LclHost *host, const char *name,
                             const char *script_path) {
  if (!host || !name || !script_path) {
    return;
  }

  // Check if already registered
  RegisteredLayer *existing = find_registered_layer(host, name);
  if (existing) {
    // Update script path
    free(existing->script_path);
    existing->script_path = lcl_strdup(script_path);
    return;
  }

  // Grow array if needed
  if (host->registered_layer_count >= host->registered_layer_cap) {
    size_t new_cap = host->registered_layer_cap * 2;
    RegisteredLayer *new_layers =
        realloc(host->registered_layers, new_cap * sizeof(RegisteredLayer));
    if (!new_layers) {
      return;
    }
    host->registered_layers = new_layers;
    host->registered_layer_cap = new_cap;
  }

  // Add new layer
  RegisteredLayer *layer =
      &host->registered_layers[host->registered_layer_count++];
  layer->name = lcl_strdup(name);
  layer->script_path = lcl_strdup(script_path);
  layer->enabled = true;
}

void lcl_host_unregister_layer(LclHost *host, const char *name) {
  if (!host || !name) {
    return;
  }

  for (size_t i = 0; i < host->registered_layer_count; i++) {
    if (host->registered_layers[i].name &&
        strcmp(host->registered_layers[i].name, name) == 0) {
      // Free this entry
      free(host->registered_layers[i].name);
      free(host->registered_layers[i].script_path);

      // Move last entry to this slot
      if (i < host->registered_layer_count - 1) {
        host->registered_layers[i] =
            host->registered_layers[host->registered_layer_count - 1];
      }
      host->registered_layer_count--;
      return;
    }
  }
}

void lcl_host_set_layer_enabled(LclHost *host, const char *name, bool enabled) {
  if (!host || !name) {
    return;
  }

  RegisteredLayer *layer = find_registered_layer(host, name);
  if (layer) {
    layer->enabled = enabled;
  }
}

int lcl_host_refresh_dirty_layers(LclHost *host) {
  if (!host || !host->current_buffer) {
    return 0;
  }

  int count = 0;
  AnnotStore *store = host->current_buffer->annots;

  for (size_t i = 0; i < host->registered_layer_count; i++) {
    RegisteredLayer *layer = &host->registered_layers[i];

    if (!layer->enabled) {
      continue;
    }

    // Check if this layer is dirty
    LayerState state = annot_store_layer_state(store, layer->name);
    if (state != LAYER_DIRTY) {
      continue;
    }

    // Run the layer script
    const char *error_msg = NULL;
    if (lcl_host_run_layer_file(host, layer->name, layer->script_path,
                                &error_msg)) {
      count++;
    } else {
      // Log error but continue with other layers
      fprintf(stderr, "Layer '%s' error: %s\n", layer->name,
              error_msg ? error_msg : "unknown");
    }
  }

  return count;
}

bool lcl_host_has_dirty_layers(LclHost *host) {
  if (!host || !host->current_buffer) {
    return false;
  }

  AnnotStore *store = host->current_buffer->annots;

  for (size_t i = 0; i < host->registered_layer_count; i++) {
    RegisteredLayer *layer = &host->registered_layers[i];

    if (!layer->enabled) {
      continue;
    }

    LayerState state = annot_store_layer_state(store, layer->name);
    if (state == LAYER_DIRTY) {
      return true;
    }
  }

  return false;
}

// === Interaction Handler Registration ===

// Helper to find registered handler by name
static RegisteredHandler *find_registered_handler(LclHost *host,
                                                  const char *name) {
  for (size_t i = 0; i < host->handler_count; i++) {
    if (host->handlers[i].name && strcmp(host->handlers[i].name, name) == 0) {
      return &host->handlers[i];
    }
  }
  return NULL;
}

// Comparison function for sorting handlers by priority (descending)
static int handler_priority_cmp(const void *a, const void *b) {
  const RegisteredHandler *ha = (const RegisteredHandler *)a;
  const RegisteredHandler *hb = (const RegisteredHandler *)b;
  return hb->priority - ha->priority; // Descending order
}

void lcl_host_register_handler(LclHost *host, const char *name,
                               HandlerEventType event_type, const char *script,
                               int priority) {
  if (!host || !name || !script) {
    return;
  }

  // Check if already registered
  RegisteredHandler *existing = find_registered_handler(host, name);
  if (existing) {
    // Update existing handler
    free(existing->script);
    existing->script = lcl_strdup(script);
    existing->event_type = event_type;
    existing->priority = priority;
    // Re-sort handlers
    qsort(host->handlers, host->handler_count, sizeof(RegisteredHandler),
          handler_priority_cmp);
    return;
  }

  // Grow array if needed
  if (host->handler_count >= host->handler_cap) {
    size_t new_cap = host->handler_cap * 2;
    RegisteredHandler *new_handlers =
        realloc(host->handlers, new_cap * sizeof(RegisteredHandler));
    if (!new_handlers) {
      return;
    }
    host->handlers = new_handlers;
    host->handler_cap = new_cap;
  }

  // Add new handler
  RegisteredHandler *handler = &host->handlers[host->handler_count++];
  handler->name = lcl_strdup(name);
  handler->event_type = event_type;
  handler->script = lcl_strdup(script);
  handler->priority = priority;
  handler->enabled = true;

  // Sort handlers by priority
  qsort(host->handlers, host->handler_count, sizeof(RegisteredHandler),
        handler_priority_cmp);
}

void lcl_host_unregister_handler(LclHost *host, const char *name) {
  if (!host || !name) {
    return;
  }

  for (size_t i = 0; i < host->handler_count; i++) {
    if (host->handlers[i].name && strcmp(host->handlers[i].name, name) == 0) {
      // Free this entry
      free(host->handlers[i].name);
      free(host->handlers[i].script);

      // Move last entry to this slot
      if (i < host->handler_count - 1) {
        host->handlers[i] = host->handlers[host->handler_count - 1];
      }
      host->handler_count--;

      // Re-sort handlers
      if (host->handler_count > 0) {
        qsort(host->handlers, host->handler_count, sizeof(RegisteredHandler),
              handler_priority_cmp);
      }
      return;
    }
  }
}

void lcl_host_set_handler_enabled(LclHost *host, const char *name,
                                  bool enabled) {
  if (!host || !name) {
    return;
  }

  RegisteredHandler *handler = find_registered_handler(host, name);
  if (handler) {
    handler->enabled = enabled;
  }
}

// === Interaction Context ===

void lcl_host_set_current_event(LclHost *host, InteractEvent *event,
                                Editor *editor) {
  if (!host) {
    return;
  }
  host->current_event = event;
  host->current_editor = editor;

  // Also set the buffer context
  if (editor) {
    host->current_buffer = editor_get_buffer(editor);
  }
}

void lcl_host_clear_current_event(LclHost *host) {
  if (!host) {
    return;
  }
  host->current_event = NULL;
  host->current_editor = NULL;
  host->current_buffer = NULL;
}

// Convert HandlerEventType to InteractType for comparison
static bool handler_type_matches_event(HandlerEventType ht, InteractType it) {
  switch (ht) {
  case HANDLER_LEFT_CLICK: return it == INTERACT_LEFT_CLICK;
  case HANDLER_MIDDLE_CLICK: return it == INTERACT_MIDDLE_CLICK;
  case HANDLER_RIGHT_CLICK: return it == INTERACT_RIGHT_CLICK;
  case HANDLER_HOVER: return it == INTERACT_HOVER;
  }
  return false;
}

bool lcl_host_dispatch_global_handlers(LclHost *host, InteractEvent *event,
                                       Editor *editor) {
  if (!host || !event) {
    return false;
  }

  // Set context before running handlers
  lcl_host_set_current_event(host, event, editor);

  // Try handlers in priority order
  for (size_t i = 0; i < host->handler_count; i++) {
    RegisteredHandler *handler = &host->handlers[i];

    if (!handler->enabled) {
      continue;
    }

    // Check if this handler matches the event type
    if (!handler_type_matches_event(handler->event_type, event->type)) {
      continue;
    }

    // Execute the handler
    const char *error_msg = NULL;
    bool ok = lcl_host_eval(host, handler->script, &error_msg);

    if (!ok) {
      const char *err_file = lcl_interp_error_file(host->interp);
      int err_line = lcl_interp_error_line(host->interp);
      fprintf(stderr, "Handler '%s' error at %s:%d: %s\n", handler->name,
              err_file ? err_file : "<unknown>", err_line,
              error_msg ? error_msg : "unknown");
    }

    // Check if handler marked event as handled
    if (event->handled) {
      lcl_host_clear_current_event(host);
      return true;
    }
  }

  lcl_host_clear_current_event(host);
  return false;
}

// === App Context ===

void lcl_host_set_app_context(LclHost *host, BufferManager *bm,
                              LayoutNode **layout_root, Pane **focused_pane,
                              void *app_ctx, int char_width, int line_height,
                              int window_width, int window_height) {
  if (!host) {
    return;
  }
  host->buffer_manager = bm;
  host->layout_root_ptr = layout_root;
  host->focused_pane_ptr = focused_pane;
  host->app_context = app_ctx;
  host->char_width = char_width;
  host->line_height = line_height;
  host->window_width = window_width;
  host->window_height = window_height;
}

void lcl_host_set_window_size(LclHost *host, int width, int height) {
  if (!host) {
    return;
  }
  host->window_width = width;
  host->window_height = height;
}

// Helper: Recompute layout bounds after adding/removing panes
static void recompute_layout_bounds(LclHost *host) {
  if (!host || !host->layout_root_ptr || !*host->layout_root_ptr) {
    return;
  }
  SDL_Rect window_rect = {
      .x = 0, .y = 0, .w = host->window_width, .h = host->window_height};
  layout_compute_bounds(*host->layout_root_ptr, window_rect);
}

// === Configuration ===

void lcl_host_register_filetype(LclHost *host, const char *extension,
                                const char *layer_name,
                                const char *script_path) {
  if (!host || !extension || !layer_name || !script_path) {
    return;
  }

  // Check if we need to grow the array
  if (host->filetype_count >= host->filetype_cap) {
    size_t new_cap = host->filetype_cap * 2;
    FileTypeAssoc *new_arr =
        realloc(host->filetypes, new_cap * sizeof(FileTypeAssoc));
    if (!new_arr) {
      return;
    }
    host->filetypes = new_arr;
    host->filetype_cap = new_cap;
  }

  // Add the association
  FileTypeAssoc *assoc = &host->filetypes[host->filetype_count];
  assoc->extension = strdup(extension);
  assoc->layer_name = strdup(layer_name);
  assoc->script_path = strdup(script_path);

  if (!assoc->extension || !assoc->layer_name || !assoc->script_path) {
    free(assoc->extension);
    free(assoc->layer_name);
    free(assoc->script_path);
    return;
  }

  host->filetype_count++;
}

void lcl_host_apply_filetypes(LclHost *host, Buffer *buffer) {
  if (!host || !buffer || !buffer->file_path) {
    return;
  }

  // Find the file extension
  const char *ext = strrchr(buffer->file_path, '.');
  if (!ext) {
    return;
  }

  // Save current buffer and set this buffer as current for layer execution
  Buffer *prev_buffer = host->current_buffer;
  host->current_buffer = buffer;

  // Look for matching file type associations
  for (size_t i = 0; i < host->filetype_count; i++) {
    FileTypeAssoc *assoc = &host->filetypes[i];
    if (strcmp(ext, assoc->extension) == 0) {
      // Register the layer for this buffer
      lcl_host_register_layer(host, assoc->layer_name, assoc->script_path);

      // Register the layer in the annotation store
      annot_store_register_layer(buffer->annots, assoc->layer_name);

      // Run the layer immediately
      const char *error_msg = NULL;
      if (!lcl_host_run_layer_file(host, assoc->layer_name, assoc->script_path,
                                   &error_msg)) {
        fprintf(stderr, "Error running layer %s: %s\n", assoc->layer_name,
                error_msg ? error_msg : "unknown");
      }
    }
  }

  // Restore previous buffer
  host->current_buffer = prev_buffer;
}

void lcl_host_apply_indent(LclHost *host, Editor *editor) {
  if (!host || !editor) {
    return;
  }

  Buffer *buffer = editor_get_buffer(editor);
  if (!buffer || !buffer->file_path) {
    return;
  }

  // Find the file extension
  const char *ext = strrchr(buffer->file_path, '.');
  if (!ext) {
    return;
  }

  // Look up indent settings for this extension
  const IndentSettings *settings = lcl_host_get_indent(host, ext);
  if (settings) {
    editor_set_indent(editor, settings->tab_size, settings->use_tabs);
  }
}

bool lcl_host_load_config(LclHost *host) {
  if (!host || !host->config_dir) {
    return true; // No config dir is not an error
  }

  // Build path to init.lcl
  size_t len = strlen(host->config_dir) + strlen("/init.lcl") + 1;
  char *init_path = malloc(len);
  if (!init_path) {
    return false;
  }
  snprintf(init_path, len, "%s/init.lcl", host->config_dir);

  // Check if file exists
  struct stat st;
  if (stat(init_path, &st) != 0) {
    // File doesn't exist - not an error
    free(init_path);
    return true;
  }

  // Execute the config file
  const char *error_msg = NULL;
  bool ok = lcl_host_eval_file(host, init_path, &error_msg);
  free(init_path);

  if (!ok) {
    fprintf(stderr, "Error loading config: %s\n",
            error_msg ? error_msg : "unknown");
    return false;
  }

  return true;
}

const char *lcl_host_get_config_dir(LclHost *host) {
  if (!host) {
    return NULL;
  }
  return host->config_dir;
}

void lcl_host_register_indent(LclHost *host, const char *extension,
                              int tab_size, bool use_tabs) {
  if (!host || !extension) {
    return;
  }

  // Check if we already have settings for this extension (update if so)
  for (size_t i = 0; i < host->indent_count; i++) {
    if (strcmp(host->indent_settings[i].extension, extension) == 0) {
      host->indent_settings[i].tab_size = tab_size;
      host->indent_settings[i].use_tabs = use_tabs;
      return;
    }
  }

  // Check if we need to grow the array
  if (host->indent_count >= host->indent_cap) {
    size_t new_cap = host->indent_cap * 2;
    IndentSettings *new_arr =
        realloc(host->indent_settings, new_cap * sizeof(IndentSettings));
    if (!new_arr) {
      return;
    }
    host->indent_settings = new_arr;
    host->indent_cap = new_cap;
  }

  // Add the new settings
  IndentSettings *settings = &host->indent_settings[host->indent_count];
  settings->extension = strdup(extension);
  if (!settings->extension) {
    return;
  }
  settings->tab_size = tab_size;
  settings->use_tabs = use_tabs;

  host->indent_count++;
}

const IndentSettings *lcl_host_get_indent(LclHost *host,
                                          const char *extension) {
  if (!host || !extension) {
    return NULL;
  }

  for (size_t i = 0; i < host->indent_count; i++) {
    if (strcmp(host->indent_settings[i].extension, extension) == 0) {
      return &host->indent_settings[i];
    }
  }

  return NULL;
}

// === Wrap Settings Functions ===

void lcl_host_register_wrap(LclHost *host, const char *extension, bool enabled,
                            int wrap_width) {
  if (!host || !extension) {
    return;
  }

  // Check if we already have settings for this extension (update if so)
  for (size_t i = 0; i < host->wrap_count; i++) {
    if (strcmp(host->wrap_settings[i].extension, extension) == 0) {
      host->wrap_settings[i].enabled = enabled;
      host->wrap_settings[i].wrap_width = wrap_width;
      return;
    }
  }

  // Check if we need to grow the array
  if (host->wrap_count >= host->wrap_cap) {
    size_t new_cap = host->wrap_cap * 2;
    WrapSettings *new_arr =
        realloc(host->wrap_settings, new_cap * sizeof(WrapSettings));
    if (!new_arr) {
      return;
    }
    host->wrap_settings = new_arr;
    host->wrap_cap = new_cap;
  }

  // Add the new settings
  WrapSettings *settings = &host->wrap_settings[host->wrap_count];
  settings->extension = strdup(extension);
  if (!settings->extension) {
    return;
  }
  settings->enabled = enabled;
  settings->wrap_width = wrap_width;

  host->wrap_count++;
}

const WrapSettings *lcl_host_get_wrap(LclHost *host, const char *extension) {
  if (!host || !extension) {
    return NULL;
  }

  for (size_t i = 0; i < host->wrap_count; i++) {
    if (strcmp(host->wrap_settings[i].extension, extension) == 0) {
      return &host->wrap_settings[i];
    }
  }

  return NULL;
}

void lcl_host_apply_wrap(LclHost *host, Editor *editor) {
  if (!host || !editor) {
    return;
  }

  Buffer *buffer = editor_get_buffer(editor);
  if (!buffer || !buffer->file_path) {
    return;
  }

  // Find the file extension
  const char *ext = strrchr(buffer->file_path, '.');
  if (!ext) {
    return;
  }

  // Look up wrap settings for this extension
  const WrapSettings *settings = lcl_host_get_wrap(host, ext);
  if (settings) {
    editor_set_wrap(editor, settings->enabled, settings->wrap_width);
  }
}

// === Font Configuration Functions ===

void lcl_host_set_font(LclHost *host, const char *path, int size) {
  if (!host) {
    return;
  }

  // Free existing path if any
  if (host->font_config.path) {
    free(host->font_config.path);
    host->font_config.path = NULL;
  }

  // Set new configuration
  if (path) {
    host->font_config.path = strdup(path);
  }
  host->font_config.size = size > 0 ? size : 14; // Default to 14 if invalid
  host->font_config_set = true;
}

const FontConfig *lcl_host_get_font(LclHost *host) {
  if (!host) {
    return NULL;
  }
  return &host->font_config;
}

bool lcl_host_has_font_config(LclHost *host) {
  if (!host) {
    return false;
  }
  return host->font_config_set;
}

TSLangRegistry *lcl_host_get_ts_registry(LclHost *host) {
  if (!host) {
    return NULL;
  }
  return host->ts_registry;
}

// === Keybinding Functions ===

void lcl_host_register_keybind(LclHost *host, int keycode, int modifiers,
                               const char *script) {
  if (!host || !script) {
    return;
  }

  // Check if we already have a binding for this key combo (update if so)
  for (size_t i = 0; i < host->keybind_count; i++) {
    if (host->keybindings[i].keycode == keycode &&
        host->keybindings[i].modifiers == modifiers) {
      free(host->keybindings[i].script);
      host->keybindings[i].script = strdup(script);
      host->keybindings[i].enabled = true;
      return;
    }
  }

  // Check if we need to grow the array
  if (host->keybind_count >= host->keybind_cap) {
    size_t new_cap = host->keybind_cap * 2;
    RegisteredKeybind *new_arr =
        realloc(host->keybindings, new_cap * sizeof(RegisteredKeybind));
    if (!new_arr) {
      return;
    }
    host->keybindings = new_arr;
    host->keybind_cap = new_cap;
  }

  // Add the new keybinding
  RegisteredKeybind *kb = &host->keybindings[host->keybind_count];
  kb->keycode = keycode;
  kb->modifiers = modifiers;
  kb->script = strdup(script);
  if (!kb->script) {
    return;
  }
  kb->enabled = true;

  host->keybind_count++;
}

bool lcl_host_dispatch_keybind(LclHost *host, int keycode, int modifiers) {
  if (!host) {
    return false;
  }

  // Find matching keybinding
  for (size_t i = 0; i < host->keybind_count; i++) {
    RegisteredKeybind *kb = &host->keybindings[i];
    if (kb->enabled && kb->keycode == keycode && kb->modifiers == modifiers) {
      // Execute the script
      const char *error_msg = NULL;
      bool success = lcl_host_eval(host, kb->script, &error_msg);
      if (!success && error_msg) {
        fprintf(stderr, "Keybinding script error: %s\n", error_msg);
      }
      return true; // Keybinding matched (regardless of success)
    }
  }

  return false;
}

// === ViewLayer Configuration Functions ===

void lcl_host_register_viewlayer(LclHost *host, const char *extension,
                                 ViewLayerType type, const char *param,
                                 int priority) {
  if (!host || !extension) {
    return;
  }

  // Check if we need to grow the array
  if (host->viewlayer_count >= host->viewlayer_cap) {
    size_t new_cap = host->viewlayer_cap * 2;
    ViewLayerConfig *new_arr =
        realloc(host->viewlayer_configs, new_cap * sizeof(ViewLayerConfig));
    if (!new_arr) {
      return;
    }
    host->viewlayer_configs = new_arr;
    host->viewlayer_cap = new_cap;
  }

  // Add the new config (we allow multiple layers per extension)
  ViewLayerConfig *config = &host->viewlayer_configs[host->viewlayer_count];
  config->extension = strdup(extension);
  if (!config->extension) {
    return;
  }
  config->type = type;
  config->param = param ? strdup(param) : NULL;
  config->priority = priority;

  host->viewlayer_count++;
}

const ViewLayerConfig *lcl_host_get_viewlayers(LclHost *host,
                                               const char *extension,
                                               size_t *out_count) {
  if (!host || !extension || !out_count) {
    if (out_count) {
      *out_count = 0;
    }
    return NULL;
  }

  // Count matching configs
  size_t count = 0;
  for (size_t i = 0; i < host->viewlayer_count; i++) {
    if (strcmp(host->viewlayer_configs[i].extension, extension) == 0) {
      count++;
    }
  }

  *out_count = count;
  if (count == 0) {
    return NULL;
  }

  // Find the first matching config
  for (size_t i = 0; i < host->viewlayer_count; i++) {
    if (strcmp(host->viewlayer_configs[i].extension, extension) == 0) {
      return &host->viewlayer_configs[i];
    }
  }

  return NULL;
}

bool lcl_host_apply_viewlayers(LclHost *host, Pane *pane) {
  if (!host || !pane || !pane->view || !pane->layer_stack) {
    return false;
  }

  Buffer *buffer = editor_get_buffer(pane->view);
  if (!buffer) {
    return false;
  }

  const char *file_path = buffer_get_file_path(buffer);
  const char *ext = NULL;

  if (file_path) {
    ext = strrchr(file_path, '.');
    // Special case: check for buffer names like "*lcl*"
    if (!ext && strcmp(file_path, "*lcl*") == 0) {
      ext = ".lcl";
    }
  }

  // Get viewlayer configs for this extension
  size_t config_count = 0;
  if (ext) {
    lcl_host_get_viewlayers(host, ext, &config_count);
  }

  // Always add a "style" annotation layer at high priority for user styling
  // This allows user annotations to override tree-sitter highlighting
  ViewLayer *style_layer = annot_layer_new("style", 100);
  if (style_layer) {
    layer_stack_add(pane->layer_stack, style_layer);
  }

  // If no custom config, we're done
  if (config_count == 0) {
    return false; // No custom config applied
  }

  // Apply each matching config
  bool applied_any = false;
  for (size_t i = 0; i < host->viewlayer_count; i++) {
    const ViewLayerConfig *config = &host->viewlayer_configs[i];
    if (!ext || strcmp(config->extension, ext) != 0) {
      continue;
    }

    ViewLayer *layer = NULL;

    switch (config->type) {
    case VIEWLAYER_TREE_SITTER:
      // Try registry first for all languages
      if (host->ts_registry) {
        layer = tree_sitter_layer_from_registry(
            host->ts_registry, config->param, config->priority);
      }
      // Fall back to hardcoded lcl support if registry doesn't have it
      // (for backwards compatibility when WEFT_EMBED_TS_LCL is disabled)
      if (!layer && strcmp(config->param, "lcl") == 0) {
        layer = tree_sitter_layer_new_lcl(config->priority);
      }
      if (!layer) {
        fprintf(stderr, "viewlayer: tree-sitter language '%s' not available\n",
                config->param);
      }
      break;

    case VIEWLAYER_LCL_SCRIPT:
      // TODO: Implement Lcl script-based viewlayer
      // This would run an Lcl script that produces decorations
      fprintf(stderr, "viewlayer: lcl-script type not yet implemented\n");
      break;

    case VIEWLAYER_ANNOT:
      // Create annotation layer that reads from annotation store
      layer = annot_layer_new(config->param, config->priority);
      break;
    }

    if (layer) {
      layer_stack_add(pane->layer_stack, layer);
      applied_any = true;
    }
  }

  return applied_any;
}

// === Accessors for interaction dispatch ===

BufferManager *lcl_host_get_buffer_manager(LclHost *host) {
  return host ? host->buffer_manager : NULL;
}

Buffer *lcl_host_get_repl_attached_buffer(LclHost *host) {
  return host ? host->repl_attached_buffer : NULL;
}

LayoutNode *lcl_host_get_layout_root(LclHost *host) {
  return (host && host->layout_root_ptr) ? *host->layout_root_ptr : NULL;
}

// === Transform Prompt ===

bool lcl_host_has_transform_prompt(LclHost *host) {
  return host && host->transform_prompt.active;
}

bool lcl_host_get_transform_prompt(LclHost *host, const char **message,
                                   const char **default_value) {
  if (!host || !host->transform_prompt.active) {
    return false;
  }

  if (message) {
    *message = host->transform_prompt.prompt_message;
  }
  if (default_value) {
    *default_value = host->transform_prompt.default_value;
  }
  return true;
}

void lcl_host_set_transform_prompt(LclHost *host, const char *transform_name,
                                   const char *message, const char *default_val,
                                   Buffer *buffer, size_t start, size_t end) {
  if (!host) {
    return;
  }

  // Clear any existing prompt
  lcl_host_cancel_transform_prompt(host);

  // Set new prompt
  host->transform_prompt.transform_name =
      transform_name ? strdup(transform_name) : NULL;
  host->transform_prompt.prompt_message = message ? strdup(message) : NULL;
  host->transform_prompt.default_value =
      default_val ? strdup(default_val) : NULL;
  host->transform_prompt.buffer = buffer;
  host->transform_prompt.start = start;
  host->transform_prompt.end = end;
  host->transform_prompt.active = true;
}

Patch *lcl_host_submit_transform_prompt(LclHost *host, const char *input) {
  if (!host || !host->transform_prompt.active) {
    return NULL;
  }

  // Get the info we need
  const char *name = host->transform_prompt.transform_name;
  Buffer *buffer = host->transform_prompt.buffer;
  size_t start = host->transform_prompt.start;
  size_t end = host->transform_prompt.end;

  if (!name || !buffer) {
    lcl_host_cancel_transform_prompt(host);
    return NULL;
  }

  // Build args array with input
  const char *args[2] = {input, NULL};

  // Clear the prompt before re-running (in case transform prompts again)
  host->transform_prompt.active = false;

  // Re-run the transform with input
  Patch *patch = transform_run(name, buffer, start, end, args);

  // Clean up prompt state
  free(host->transform_prompt.transform_name);
  free(host->transform_prompt.prompt_message);
  free(host->transform_prompt.default_value);
  host->transform_prompt.transform_name = NULL;
  host->transform_prompt.prompt_message = NULL;
  host->transform_prompt.default_value = NULL;
  host->transform_prompt.buffer = NULL;

  // If we got a patch, show the diff preview and store it
  if (patch) {
    // Store pending patch
    if (host->pending_patch) {
      patch_free(host->pending_patch);
    }
    host->pending_patch = patch;
    host->patch_target_buffer = buffer;

    // Show the diff buffer
    show_patch_buffer(host, patch, buffer);
  }

  return patch;
}

void lcl_host_cancel_transform_prompt(LclHost *host) {
  if (!host) {
    return;
  }

  free(host->transform_prompt.transform_name);
  free(host->transform_prompt.prompt_message);
  free(host->transform_prompt.default_value);

  host->transform_prompt.transform_name = NULL;
  host->transform_prompt.prompt_message = NULL;
  host->transform_prompt.default_value = NULL;
  host->transform_prompt.buffer = NULL;
  host->transform_prompt.start = 0;
  host->transform_prompt.end = 0;
  host->transform_prompt.active = false;
}

// === Annotation Persistence ===

const char *lcl_host_get_data_dir(LclHost *host) {
  return host ? host->data_dir : NULL;
}

DocRegistry *lcl_host_get_doc_registry(LclHost *host) {
  return host ? host->doc_registry : NULL;
}

void lcl_host_set_layer_persistent(LclHost *host, const char *layer) {
  if (!host || !layer) {
    return;
  }

  // Check if already in list
  for (size_t i = 0; i < host->persistent_layer_count; i++) {
    if (strcmp(host->persistent_layers[i], layer) == 0) {
      return; // Already persistent
    }
  }

  // Grow array if needed
  if (host->persistent_layer_count >= host->persistent_layer_cap) {
    size_t new_cap =
        host->persistent_layer_cap == 0 ? 8 : host->persistent_layer_cap * 2;
    char **new_layers =
        realloc(host->persistent_layers, new_cap * sizeof(char *));
    if (!new_layers) {
      return;
    }
    host->persistent_layers = new_layers;
    host->persistent_layer_cap = new_cap;
  }

  host->persistent_layers[host->persistent_layer_count] = strdup(layer);
  if (host->persistent_layers[host->persistent_layer_count]) {
    host->persistent_layer_count++;
  }
}

void lcl_host_set_layer_ephemeral(LclHost *host, const char *layer) {
  if (!host || !layer) {
    return;
  }

  // Find and remove from persistent list
  for (size_t i = 0; i < host->persistent_layer_count; i++) {
    if (strcmp(host->persistent_layers[i], layer) == 0) {
      free(host->persistent_layers[i]);
      // Shift remaining
      for (size_t j = i; j < host->persistent_layer_count - 1; j++) {
        host->persistent_layers[j] = host->persistent_layers[j + 1];
      }
      host->persistent_layer_count--;
      return;
    }
  }
}

bool lcl_host_is_layer_persistent(LclHost *host, const char *layer) {
  if (!host || !layer) {
    return false;
  }

  // If no explicit list, use default behavior
  if (host->persistent_layer_count == 0) {
    // Default: ts/plumb layers are ephemeral (regenerated from content)
    // The "style" layer is persistent for user annotations
    if (strcmp(layer, "ts") == 0 || strcmp(layer, "plumb") == 0) {
      return false;
    }
    return true;
  }

  // Check explicit list
  for (size_t i = 0; i < host->persistent_layer_count; i++) {
    if (strcmp(host->persistent_layers[i], layer) == 0) {
      return true;
    }
  }
  return false;
}

const char *const *lcl_host_get_persistent_layers(LclHost *host,
                                                  size_t *out_count) {
  if (!host) {
    if (out_count) {
      *out_count = 0;
    }
    return NULL;
  }

  if (out_count) {
    *out_count = host->persistent_layer_count;
  }
  return (const char *const *)host->persistent_layers;
}

bool lcl_host_save_registry(LclHost *host) {
  if (!host || !host->doc_registry) {
    return false;
  }
  return doc_registry_save(host->doc_registry);
}
