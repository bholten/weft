#include "tree_sitter_hl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-lcl.h>

#include "buffer.h"
#include "document.h"
#include "annot_store.h"

// === Color Scheme ===

// Map capture names to colors
typedef struct {
    const char *capture;    // Without @ prefix
    const char *color;
} CaptureColor;

static const CaptureColor DEFAULT_COLORS[] = {
    // Comments
    {"comment",             "#6A9955"},

    // Strings
    {"string",              "#CE9178"},
    {"string.special",      "#D7BA7D"},

    // Keywords
    {"keyword",             "#C586C0"},
    {"keyword.function",    "#C586C0"},
    {"keyword.return",      "#C586C0"},
    {"keyword.conditional", "#C586C0"},
    {"keyword.repeat",      "#C586C0"},
    {"keyword.operator",    "#C586C0"},

    // Functions
    {"function",            "#DCDCAA"},
    {"function.builtin",    "#DCDCAA"},
    {"function.call",       "#DCDCAA"},
    {"method",              "#DCDCAA"},

    // Variables
    {"variable",            "#9CDCFE"},
    {"variable.parameter",  "#9CDCFE"},
    {"variable.builtin",    "#569CD6"},
    {"parameter",           "#9CDCFE"},

    // Constants
    {"constant",            "#4FC1FF"},
    {"constant.builtin",    "#569CD6"},
    {"number",              "#B5CEA8"},
    {"boolean",             "#569CD6"},

    // Types
    {"type",                "#4EC9B0"},
    {"type.builtin",        "#4EC9B0"},

    // Operators
    {"operator",            "#D4D4D4"},

    // Punctuation
    {"punctuation",         "#D4D4D4"},
    {"punctuation.bracket", "#FFD700"},
    {"punctuation.delimiter", "#D4D4D4"},

    // Properties
    {"property",            "#9CDCFE"},
    {"attribute",           "#9CDCFE"},

    // Namespace
    {"namespace",           "#4EC9B0"},

    // Tags (for markup)
    {"tag",                 "#569CD6"},

    {NULL, NULL}
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

    return NULL;  // No color for this capture
}

// === Highlighter Implementation ===

TSHighlighter *ts_hl_new(const TSLanguage *language, const char *language_name,
                          const char *query_source) {
    if (!language || !query_source) {
        return NULL;
    }

    TSHighlighter *hl = calloc(1, sizeof(TSHighlighter));
    if (!hl) {
        return NULL;
    }

    hl->language = language;
    hl->language_name = language_name ? strdup(language_name) : NULL;
    hl->query_source = strdup(query_source);

    // Create parser
    hl->parser = ts_parser_new();
    if (!hl->parser) {
        ts_hl_free(hl);
        return NULL;
    }

    if (!ts_parser_set_language(hl->parser, language)) {
        fprintf(stderr, "tree-sitter: failed to set language\n");
        ts_hl_free(hl);
        return NULL;
    }

    // Create query
    uint32_t error_offset;
    TSQueryError error_type;
    hl->query = ts_query_new(language, query_source, strlen(query_source),
                              &error_offset, &error_type);
    if (!hl->query) {
        fprintf(stderr, "tree-sitter: query error at offset %u: ", error_offset);
        switch (error_type) {
            case TSQueryErrorSyntax:    fprintf(stderr, "syntax error\n"); break;
            case TSQueryErrorNodeType:  fprintf(stderr, "invalid node type\n"); break;
            case TSQueryErrorField:     fprintf(stderr, "invalid field\n"); break;
            case TSQueryErrorCapture:   fprintf(stderr, "invalid capture\n"); break;
            default:                    fprintf(stderr, "unknown error\n"); break;
        }
        ts_hl_free(hl);
        return NULL;
    }

    return hl;
}

void ts_hl_free(TSHighlighter *hl) {
    if (!hl) {
        return;
    }

    if (hl->tree) {
        ts_tree_delete(hl->tree);
    }
    if (hl->query) {
        ts_query_delete(hl->query);
    }
    if (hl->parser) {
        ts_parser_delete(hl->parser);
    }
    free(hl->language_name);
    free(hl->query_source);
    free(hl);
}

bool ts_hl_parse(TSHighlighter *hl, const char *source, size_t source_len) {
    if (!hl || !hl->parser || !source) {
        return false;
    }

    // Parse (with old tree for incremental parsing if available)
    TSTree *old_tree = hl->tree;
    hl->tree = ts_parser_parse_string(hl->parser, old_tree, source, (uint32_t)source_len);

    if (old_tree) {
        ts_tree_delete(old_tree);
    }

    return hl->tree != NULL;
}

TSHighlightResult *ts_hl_highlight(TSHighlighter *hl) {
    if (!hl || !hl->tree || !hl->query) {
        return NULL;
    }

    TSHighlightResult *result = calloc(1, sizeof(TSHighlightResult));
    if (!result) {
        return NULL;
    }

    // Initial capacity
    result->capacity = 256;
    result->highlights = malloc(result->capacity * sizeof(TSHighlight));
    if (!result->highlights) {
        free(result);
        return NULL;
    }

    // Create query cursor
    TSQueryCursor *cursor = ts_query_cursor_new();
    if (!cursor) {
        ts_hl_result_free(result);
        return NULL;
    }

    TSNode root = ts_tree_root_node(hl->tree);
    ts_query_cursor_exec(cursor, hl->query, root);

    // Iterate matches
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        for (uint16_t i = 0; i < match.capture_count; i++) {
            TSQueryCapture capture = match.captures[i];
            TSNode node = capture.node;

            uint32_t capture_name_len;
            const char *capture_name = ts_query_capture_name_for_id(
                hl->query, capture.index, &capture_name_len);

            // Grow array if needed
            if (result->count >= result->capacity) {
                result->capacity *= 2;
                TSHighlight *new_highlights = realloc(result->highlights,
                    result->capacity * sizeof(TSHighlight));
                if (!new_highlights) {
                    ts_query_cursor_delete(cursor);
                    ts_hl_result_free(result);
                    return NULL;
                }
                result->highlights = new_highlights;
            }

            // Add highlight
            TSHighlight *h = &result->highlights[result->count++];
            h->start = ts_node_start_byte(node);
            h->end = ts_node_end_byte(node);
            h->capture = capture_name;  // Points to query's internal storage
        }
    }

    ts_query_cursor_delete(cursor);
    return result;
}

void ts_hl_result_free(TSHighlightResult *result) {
    if (!result) {
        return;
    }
    free(result->highlights);
    free(result);
}

void ts_hl_apply_to_buffer(TSHighlighter *hl, Buffer *buffer,
                           const char *script_path) {
    if (!hl || !buffer) {
        return;
    }

    // Get document text
    Document *doc = buffer->document;
    size_t doc_len = document_length(doc);

    char *text = malloc(doc_len + 1);
    if (!text) {
        return;
    }
    document_get_text(doc, 0, text, doc_len);
    text[doc_len] = '\0';

    // Parse
    if (!ts_hl_parse(hl, text, doc_len)) {
        free(text);
        return;
    }
    free(text);

    // Get highlights
    TSHighlightResult *result = ts_hl_highlight(hl);
    if (!result) {
        return;
    }

    // Clear existing style layer
    annot_store_clear_layer(buffer->annots, "style");

    // Add annotations for each highlight
    for (size_t i = 0; i < result->count; i++) {
        TSHighlight *h = &result->highlights[i];

        const char *color = get_color_for_capture(h->capture);
        if (!color) {
            continue;  // No color for this capture
        }

        // Build metadata
        const char *keys[2];
        const char *values[2];
        size_t meta_count = 1;

        keys[0] = "style:fg";
        values[0] = color;

        // Add provenance if available
        if (script_path) {
            keys[meta_count] = "prov:script";
            values[meta_count] = script_path;
            meta_count++;
        }

        annot_store_add(buffer->annots, h->start, h->end, "style",
                       keys, values, meta_count);
    }

    ts_hl_result_free(result);
}

// === Built-in Languages ===

static TSHighlighter *g_lcl_highlighter = NULL;

TSHighlighter *ts_hl_get_lcl(const char *highlights_path) {
    if (g_lcl_highlighter) {
        return g_lcl_highlighter;
    }

    // Read highlights.scm
    FILE *f = fopen(highlights_path, "r");
    if (!f) {
        fprintf(stderr, "tree-sitter: cannot open %s\n", highlights_path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *query_source = malloc(size + 1);
    if (!query_source) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(query_source, 1, size, f);
    fclose(f);
    query_source[read] = '\0';

    // Create highlighter
    g_lcl_highlighter = ts_hl_new(tree_sitter_lcl(), "lcl", query_source);
    free(query_source);

    if (!g_lcl_highlighter) {
        fprintf(stderr, "tree-sitter: failed to create Lcl highlighter\n");
    }

    return g_lcl_highlighter;
}

void ts_hl_cleanup(void) {
    if (g_lcl_highlighter) {
        ts_hl_free(g_lcl_highlighter);
        g_lcl_highlighter = NULL;
    }
}
