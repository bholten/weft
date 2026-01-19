#ifndef WEFT_LAYER_H
#define WEFT_LAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "decoration.h"

// Forward declarations
typedef struct Buffer Buffer;
typedef struct ViewLayer ViewLayer;

// ViewLayer render callback: produces decorations for a viewport range
// Called when the layer stack needs to render visible content.
//
// Arguments:
//   layer         - The layer instance
//   buffer        - The buffer being rendered
//   viewport_start - Start byte offset of visible region
//   viewport_end   - End byte offset of visible region (exclusive)
//   out           - DecorationList to append decorations to
//
// The callback should append decorations to 'out' for the given viewport.
// Decorations outside the viewport may be ignored by the renderer.
typedef void (*ViewLayerRenderFn)(ViewLayer *layer, Buffer *buffer,
                                   size_t viewport_start, size_t viewport_end,
                                   DecorationList *out);

// ViewLayer update callback: react to document edits (optional)
// Called after the document is modified.
//
// Arguments:
//   layer       - The layer instance
//   buffer      - The buffer that was edited
//   edit_pos    - Position where edit occurred
//   old_len     - Length of text that was deleted (0 for pure insert)
//   new_len     - Length of text that was inserted (0 for pure delete)
//
// This callback can update internal layer state/caches.
// Return true if the layer needs a full re-render, false if incremental update suffices.
typedef bool (*ViewLayerUpdateFn)(ViewLayer *layer, Buffer *buffer,
                                   size_t edit_pos, size_t old_len, size_t new_len);

// ViewLayer cleanup callback: free layer-specific resources
typedef void (*ViewLayerCleanupFn)(ViewLayer *layer);

// ViewLayer: a projection that computes decorations over buffer text
// Named ViewLayer to distinguish from annot_store.h's Layer (annotation namespaces)
typedef struct ViewLayer {
    char *name;               // Layer name (owned)
    int priority;             // Higher priority = rendered on top (default: 0)

    // Callbacks
    ViewLayerRenderFn render;     // Required: produce decorations
    ViewLayerUpdateFn update;     // Optional: react to edits
    ViewLayerCleanupFn cleanup;   // Optional: free resources

    // User data for callbacks (e.g., Lcl thunk, tree-sitter state)
    void *user_data;

    // Cache state
    uint64_t last_doc_rev;    // Document revision when last rendered
    bool needs_rerender;      // Hint that full rerender is needed
} ViewLayer;

// === ViewLayer Lifecycle ===

// Create a new layer with the given name (copies name)
ViewLayer *view_layer_new(const char *name);

// Free a layer and its resources
void view_layer_free(ViewLayer *layer);

// Set the render callback (required for all layers)
void view_layer_set_render(ViewLayer *layer, ViewLayerRenderFn render);

// Set the update callback (optional)
void view_layer_set_update(ViewLayer *layer, ViewLayerUpdateFn update);

// Set the cleanup callback (optional)
void view_layer_set_cleanup(ViewLayer *layer, ViewLayerCleanupFn cleanup);

// Set user data for callbacks
void view_layer_set_user_data(ViewLayer *layer, void *data);

// === ViewLayer Operations ===

// Render the layer for a viewport, appending decorations to out
void view_layer_render(ViewLayer *layer, Buffer *buffer,
                       size_t viewport_start, size_t viewport_end,
                       DecorationList *out);

// Notify layer of a document edit
// Returns true if layer needs full rerender
bool view_layer_notify_edit(ViewLayer *layer, Buffer *buffer,
                            size_t edit_pos, size_t old_len, size_t new_len);

// Mark layer as needing rerender
void view_layer_invalidate(ViewLayer *layer);

// Check if layer needs rerender (based on doc revision or invalidation)
bool view_layer_needs_render(const ViewLayer *layer, uint64_t current_doc_rev);

#endif // WEFT_LAYER_H
