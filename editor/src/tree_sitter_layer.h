#ifndef WEFT_TREE_SITTER_LAYER_H
#define WEFT_TREE_SITTER_LAYER_H

#include "layer.h"

// Forward declarations
typedef struct TSLanguage TSLanguage;
typedef struct TSLangRegistry TSLangRegistry;

// TreeSitterLayer: computes syntax highlighting decorations using tree-sitter
//
// Unlike the old approach which wrote annotations to the AnnotStore, this layer
// computes decorations on-the-fly during rendering. The tree-sitter parse tree
// is cached and incrementally updated when the document changes.
//
// Usage:
//   ViewLayer *layer = tree_sitter_layer_new("lcl", tree_sitter_lcl(),
//                                             query_source, 0);
//   layer_stack_add(stack, layer);

// Create a TreeSitterLayer for the given language
// Parameters:
//   language_name  - Name for display/debugging (e.g., "lcl", "c")
//   language       - Tree-sitter language from grammar
//   query_source   - Contents of highlights.scm query file
//   priority       - Layer priority (higher = rendered later/on top)
// Returns NULL on failure (invalid language or query)
ViewLayer *tree_sitter_layer_new(const char *language_name,
                                 const TSLanguage *language,
                                 const char *query_source, int priority);

// Get the language name for a TreeSitterLayer
// Returns NULL if not a tree-sitter layer
const char *tree_sitter_layer_get_language(const ViewLayer *layer);

// === Registry-based Layer Creation ===

// Create a TreeSitterLayer for a language registered in the registry
// Loads the language and query from the registry (dynamic or embedded)
// Parameters:
//   reg       - The language registry
//   lang_name - Name of the registered language (e.g., "c", "javascript")
//   priority  - Layer priority (higher = rendered later/on top)
// Returns NULL if language not found or loading fails
ViewLayer *tree_sitter_layer_from_registry(TSLangRegistry *reg,
                                           const char *lang_name, int priority);

// === Built-in Language Layers ===

// Create a Lcl syntax highlighting layer
// Searches for highlights.scm in common locations
// Returns NULL if highlights.scm cannot be found or layer creation fails
ViewLayer *tree_sitter_layer_new_lcl(int priority);

#endif // WEFT_TREE_SITTER_LAYER_H
