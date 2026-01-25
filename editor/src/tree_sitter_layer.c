#include "tree_sitter_layer.h"
#include "ts_lang_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <limits.h>

#include <tree_sitter/api.h>

#ifdef WEFT_EMBED_TS_LCL
#include <tree_sitter/tree-sitter-lcl.h>
#endif

#include "buffer.h"

// Forward declaration
static char *read_file(const char *path);
#include "color.h"
#include "document.h"

// === Color Scheme ===
// Map capture names to colors (VS Code-like theme)

typedef struct {
  const char *capture; // Without @ prefix
  const char *color;
} CaptureColor;

static const CaptureColor DEFAULT_COLORS[] = {
    // Comments
    {"comment",               "#6A9955"},

    // Strings
    {"string",                "#CE9178"},
    {"string.special",        "#D7BA7D"},

    // Keywords
    {"keyword",               "#C586C0"},
    {"keyword.function",      "#C586C0"},
    {"keyword.return",        "#C586C0"},
    {"keyword.conditional",   "#C586C0"},
    {"keyword.repeat",        "#C586C0"},
    {"keyword.operator",      "#C586C0"},

    // Functions
    {"function",              "#DCDCAA"},
    {"function.builtin",      "#DCDCAA"},
    {"function.call",         "#DCDCAA"},
    {"method",                "#DCDCAA"},

    // Variables
    {"variable",              "#9CDCFE"},
    {"variable.parameter",    "#9CDCFE"},
    {"variable.builtin",      "#569CD6"},
    {"parameter",             "#9CDCFE"},

    // Constants
    {"constant",              "#4FC1FF"},
    {"constant.builtin",      "#569CD6"},
    {"number",                "#B5CEA8"},
    {"boolean",               "#569CD6"},

    // Types
    {"type",                  "#4EC9B0"},
    {"type.builtin",          "#4EC9B0"},

    // Operators
    {"operator",              "#D4D4D4"},

    // Punctuation
    {"punctuation",           "#D4D4D4"},
    {"punctuation.bracket",   "#FFD700"},
    {"punctuation.delimiter", "#D4D4D4"},

    // Properties
    {"property",              "#9CDCFE"},
    {"attribute",             "#9CDCFE"},

    // Namespace
    {"namespace",             "#4EC9B0"},

    // Tags (for markup)
    {"tag",                   "#569CD6"},

    {NULL,                    NULL     }
};

static const char *get_color_for_capture(const char *capture) {
  // Strip @ prefix if present
  if (capture[0] == '@') {
    capture++;
  }

  // Try exact match first
  for (const CaptureColor *cc = DEFAULT_COLORS; cc->capture; cc++) {
    if (strcmp(cc->capture, capture) == 0) {
      return cc->color;
    }
  }

  // Try prefix match (e.g., "keyword.function" matches "keyword")
  for (const CaptureColor *cc = DEFAULT_COLORS; cc->capture; cc++) {
    size_t len = strlen(cc->capture);
    if (strncmp(capture, cc->capture, len) == 0 &&
        (capture[len] == '\0' || capture[len] == '.')) {
      return cc->color;
    }
  }

  return NULL; // No color for this capture
}

// === TreeSitterLayer Data ===

typedef struct {
  char *language_name;
  TSParser *parser;
  TSTree *tree;
  TSQuery *query;
  const TSLanguage *language;

  // Cached document state
  uint64_t parsed_doc_rev; // Document revision when tree was built
  size_t parsed_doc_len;   // Document length when parsed
} TreeSitterLayerData;

// === Render Callback ===

static void tree_sitter_layer_render(ViewLayer *layer, Buffer *buffer,
                                     size_t viewport_start, size_t viewport_end,
                                     DecorationList *out) {
  if (!layer || !layer->user_data || !buffer || !buffer->document) {
    return;
  }

  TreeSitterLayerData *data = layer->user_data;
  Document *doc = buffer->document;
  size_t doc_len = document_length(doc);
  uint64_t doc_rev = buffer_get_doc_rev(buffer);

  // Check if we need to reparse
  bool needs_reparse = (data->tree == NULL) ||
                       (data->parsed_doc_rev != doc_rev) ||
                       (data->parsed_doc_len != doc_len);

  if (needs_reparse) {
    // Get full document text for parsing
    char *text = malloc(doc_len + 1);
    if (!text) {
      return;
    }
    document_get_text(doc, 0, text, doc_len);
    text[doc_len] = '\0';

    // TODO: Optimize with incremental parsing
    // Currently we do a full reparse on every edit. To enable incremental
    // parsing:
    // 1. Wire up layer_stack_notify_edit() to be called from editor on document
    // changes
    // 2. Call ts_tree_edit() in tree_sitter_layer_update() with proper edit
    // info
    // 3. Pass old tree to ts_parser_parse_string() for incremental reuse
    // For now, full reparse is correct but less efficient.
    if (data->tree) {
      ts_tree_delete(data->tree);
      data->tree = NULL;
    }
    data->tree =
        ts_parser_parse_string(data->parser, NULL, text, (uint32_t)doc_len);
    free(text);

    if (!data->tree) {
      return; // Parse failed
    }

    data->parsed_doc_rev = doc_rev;
    data->parsed_doc_len = doc_len;
  }

  // Create query cursor
  TSQueryCursor *cursor = ts_query_cursor_new();
  if (!cursor) {
    return;
  }

  // Set byte range to limit query to viewport (with some padding for context)
  size_t range_start = (viewport_start > 256) ? (viewport_start - 256) : 0;
  size_t range_end = viewport_end + 256;
  if (range_end > doc_len) {
    range_end = doc_len;
  }
  ts_query_cursor_set_byte_range(cursor, (uint32_t)range_start,
                                 (uint32_t)range_end);

  // Execute query
  TSNode root = ts_tree_root_node(data->tree);
  ts_query_cursor_exec(cursor, data->query, root);

  // Track pattern indices for each span to handle precedence
  // Later patterns in the query file should override earlier ones
  // We use a simple approach: collect all captures, then add decorations
  // in pattern order so later patterns override earlier ones
  typedef struct {
    uint32_t start, end;
    uint32_t pattern_index;
    const char *color;
  } CaptureInfo;

  CaptureInfo *captures = NULL;
  size_t capture_count = 0;
  size_t capture_capacity = 0;

  // Collect all captures
  TSQueryMatch match;
  while (ts_query_cursor_next_match(cursor, &match)) {
    for (uint16_t i = 0; i < match.capture_count; i++) {
      TSQueryCapture capture = match.captures[i];
      TSNode node = capture.node;

      uint32_t start = ts_node_start_byte(node);
      uint32_t end = ts_node_end_byte(node);

      // Skip if outside viewport
      if (end < viewport_start || start > viewport_end) {
        continue;
      }

      // Get capture name and color
      uint32_t capture_name_len;
      const char *capture_name = ts_query_capture_name_for_id(
          data->query, capture.index, &capture_name_len);

      const char *color = get_color_for_capture(capture_name);
      if (!color) {
        continue; // No color for this capture
      }

      // Grow array if needed
      if (capture_count >= capture_capacity) {
        size_t new_cap = capture_capacity == 0 ? 64 : capture_capacity * 2;
        CaptureInfo *new_captures =
            realloc(captures, new_cap * sizeof(CaptureInfo));
        if (!new_captures) {
          free(captures);
          ts_query_cursor_delete(cursor);
          return;
        }
        captures = new_captures;
        capture_capacity = new_cap;
      }

      captures[capture_count++] =
          (CaptureInfo){.start = start,
                        .end = end,
                        .pattern_index = match.pattern_index,
                        .color = color};
    }
  }

  // Sort by (start, pattern_index) so later patterns come after earlier ones
  // for the same start position
  for (size_t i = 1; i < capture_count; i++) {
    CaptureInfo key = captures[i];
    size_t j = i;
    while (j > 0 && (captures[j - 1].start > key.start ||
                     (captures[j - 1].start == key.start &&
                      captures[j - 1].pattern_index > key.pattern_index))) {
      captures[j] = captures[j - 1];
      j--;
    }
    captures[j] = key;
  }

  // Add decorations - later patterns override earlier ones for overlapping
  // spans
  for (size_t i = 0; i < capture_count; i++) {
    Decoration *d =
        decoration_list_add(out, captures[i].start, captures[i].end);
    if (d) {
      if (color_parse_hex(captures[i].color, &d->fg)) {
        d->has_fg = true;
      }
      d->layer_name = layer->name;
    }
  }

  free(captures);

  ts_query_cursor_delete(cursor);
}

// === Update Callback (for incremental parsing) ===

static bool tree_sitter_layer_update(ViewLayer *layer, Buffer *buffer,
                                     size_t edit_pos, size_t old_len,
                                     size_t new_len) {
  if (!layer || !layer->user_data) {
    return true; // Need re-render
  }

  TreeSitterLayerData *data = layer->user_data;

  // If we have a tree, apply the edit for incremental parsing
  if (data->tree) {
    // Calculate edit parameters for tree-sitter
    TSInputEdit edit = {
        .start_byte = (uint32_t)edit_pos,
        .old_end_byte = (uint32_t)(edit_pos + old_len),
        .new_end_byte = (uint32_t)(edit_pos + new_len),
        // For simplicity, we don't track row/column - tree-sitter can recompute
        .start_point = {0, 0},
        .old_end_point = {0, 0},
        .new_end_point = {0, 0},
    };

    ts_tree_edit(data->tree, &edit);
  }

  (void)buffer; // Will be used in next render
  return true;  // Always need re-render after edit
}

// === Cleanup Callback ===

static void tree_sitter_layer_cleanup(ViewLayer *layer) {
  if (!layer || !layer->user_data) {
    return;
  }

  TreeSitterLayerData *data = layer->user_data;

  if (data->tree) {
    ts_tree_delete(data->tree);
  }
  if (data->query) {
    ts_query_delete(data->query);
  }
  if (data->parser) {
    ts_parser_delete(data->parser);
  }
  free(data->language_name);
  free(data);

  layer->user_data = NULL;
}

// === Public API ===

ViewLayer *tree_sitter_layer_new(const char *language_name,
                                 const TSLanguage *language,
                                 const char *query_source, int priority) {
  if (!language || !query_source) {
    return NULL;
  }

  // Create layer data
  TreeSitterLayerData *data = calloc(1, sizeof(TreeSitterLayerData));
  if (!data) {
    return NULL;
  }

  data->language = language;
  data->language_name = language_name ? strdup(language_name) : NULL;

  // Create parser
  data->parser = ts_parser_new();
  if (!data->parser) {
    free(data->language_name);
    free(data);
    return NULL;
  }

  if (!ts_parser_set_language(data->parser, language)) {
    fprintf(stderr, "tree-sitter-layer: failed to set language\n");
    ts_parser_delete(data->parser);
    free(data->language_name);
    free(data);
    return NULL;
  }

  // Create query
  uint32_t error_offset;
  TSQueryError error_type;
  data->query = ts_query_new(language, query_source, strlen(query_source),
                             &error_offset, &error_type);
  if (!data->query) {
    fprintf(stderr,
            "tree-sitter-layer: query error at offset %u: ", error_offset);
    switch (error_type) {
    case TSQueryErrorSyntax: fprintf(stderr, "syntax error\n"); break;
    case TSQueryErrorNodeType: fprintf(stderr, "invalid node type\n"); break;
    case TSQueryErrorField: fprintf(stderr, "invalid field\n"); break;
    case TSQueryErrorCapture: fprintf(stderr, "invalid capture\n"); break;
    default: fprintf(stderr, "unknown error\n"); break;
    }
    ts_parser_delete(data->parser);
    free(data->language_name);
    free(data);
    return NULL;
  }

  // Create ViewLayer
  char layer_name[64];
  snprintf(layer_name, sizeof(layer_name), "ts:%s",
           language_name ? language_name : "unknown");

  ViewLayer *layer = view_layer_new(layer_name);
  if (!layer) {
    ts_query_delete(data->query);
    ts_parser_delete(data->parser);
    free(data->language_name);
    free(data);
    return NULL;
  }

  layer->priority = priority;
  layer->user_data = data;

  view_layer_set_render(layer, tree_sitter_layer_render);
  view_layer_set_update(layer, tree_sitter_layer_update);
  view_layer_set_cleanup(layer, tree_sitter_layer_cleanup);

  return layer;
}

const char *tree_sitter_layer_get_language(const ViewLayer *layer) {
  if (!layer || !layer->user_data) {
    return NULL;
  }

  // Check if this looks like a tree-sitter layer by name prefix
  if (!layer->name || strncmp(layer->name, "ts:", 3) != 0) {
    return NULL;
  }

  TreeSitterLayerData *data = layer->user_data;
  return data->language_name;
}

// === Registry-based Layer Creation ===

ViewLayer *tree_sitter_layer_from_registry(TSLangRegistry *reg,
                                           const char *lang_name,
                                           int priority) {
  if (!reg || !lang_name) {
    return NULL;
  }

  // Get language config from registry
  TSLangConfig *config = ts_lang_registry_get(reg, lang_name);
  if (!config) {
    fprintf(stderr, "tree_sitter_layer: language '%s' not found in registry\n",
            lang_name);
    return NULL;
  }

  // Try to load the language if not already loaded
  if (!config->language && !config->load_failed) {
    // For dynamic languages, we need to load them
    // The registry's get_highlighter does this, but we need just the language
    // So we'll trigger loading by attempting to get highlighter (which caches)
    TSHighlighter *hl = ts_lang_registry_get_highlighter(reg, config);
    if (!hl) {
      fprintf(stderr,
              "tree_sitter_layer: failed to load language '%s' from registry\n",
              lang_name);
      return NULL;
    }
    // Now config->language should be set
  }

  if (!config->language) {
    fprintf(stderr, "tree_sitter_layer: language '%s' has no TSLanguage\n",
            lang_name);
    return NULL;
  }

  // Load query source
  char *query_source = NULL;

  if (config->source == TS_LANG_SOURCE_EMBEDDED && config->query_path) {
    // For embedded, query_path might be actual query content
    if (config->query_path[0] == '(' || config->query_path[0] == ';' ||
        config->query_path[0] == '[') {
      query_source = strdup(config->query_path);
    } else {
      query_source = read_file(config->query_path);
    }
  } else if (config->query_path) {
    query_source = read_file(config->query_path);
  }

  if (!query_source) {
    fprintf(stderr, "tree_sitter_layer: failed to load query for '%s'\n",
            lang_name);
    return NULL;
  }

  // Create the layer using the standard function
  ViewLayer *layer = tree_sitter_layer_new(lang_name, config->language,
                                           query_source, priority);
  free(query_source);

  return layer;
}

// === Built-in Language Layers ===

// Helper to read entire file into string
static char *read_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *content = malloc(size + 1);
  if (!content) {
    fclose(f);
    return NULL;
  }

  size_t read = fread(content, 1, size, f);
  fclose(f);
  content[read] = '\0';

  return content;
}

#ifdef WEFT_EMBED_TS_LCL
ViewLayer *tree_sitter_layer_new_lcl(int priority) {
  // Search for highlights.scm - prefer absolute path from CMake define
  const char *paths[] = {
#ifdef WEFT_SUBMODULES_DIR
      WEFT_SUBMODULES_DIR "/tree-sitter-lcl/queries/highlights.scm",
#endif
      "../submodules/tree-sitter-lcl/queries/highlights.scm",
      "../../submodules/tree-sitter-lcl/queries/highlights.scm", NULL};

  char *query_source = NULL;
  for (const char **p = paths; *p; p++) {
    query_source = read_file(*p);
    if (query_source) {
      break;
    }
  }

  if (!query_source) {
    fprintf(stderr, "tree_sitter_layer_new_lcl: cannot find highlights.scm\n");
    return NULL;
  }

  ViewLayer *layer =
      tree_sitter_layer_new("lcl", tree_sitter_lcl(), query_source, priority);
  free(query_source);

  return layer;
}
#else
ViewLayer *tree_sitter_layer_new_lcl(int priority) {
  (void)priority;
  // Lcl parser not embedded - return NULL
  return NULL;
}
#endif
