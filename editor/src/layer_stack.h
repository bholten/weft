#ifndef WEFT_LAYER_STACK_H
#define WEFT_LAYER_STACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "decoration.h"
#include "interaction.h"
#include "layer.h"

// Forward declarations
typedef struct Buffer Buffer;

// LayerStack: ordered composition of view layers
// Manages layer ordering, rendering, and action resolution.
typedef struct LayerStack {
    ViewLayer **layers;       // Array of layer pointers (not owned)
    size_t count;
    size_t capacity;

    // Cached render output
    DecorationList cache;     // Merged decorations for last render
    uint64_t cache_doc_rev;   // Document revision when cached
    size_t cache_start;       // Viewport start when cached
    size_t cache_end;         // Viewport end when cached
    bool cache_valid;         // Is cache usable?
} LayerStack;

// === LayerStack Lifecycle ===

// Create a new layer stack
LayerStack *layer_stack_new(void);

// Free a layer stack (does NOT free the layers themselves)
void layer_stack_free(LayerStack *stack);

// === Layer Management ===

// Add a layer to the stack (does not take ownership)
// Layers are kept sorted by priority (higher priority = later in render order)
bool layer_stack_add(LayerStack *stack, ViewLayer *layer);

// Remove a layer from the stack
bool layer_stack_remove(LayerStack *stack, ViewLayer *layer);

// Find a layer by name (returns NULL if not found)
ViewLayer *layer_stack_find(const LayerStack *stack, const char *name);

// Get layer count
size_t layer_stack_count(const LayerStack *stack);

// Get layer at index (for iteration)
ViewLayer *layer_stack_get(const LayerStack *stack, size_t index);

// === Rendering ===

// Render all layers for a viewport, returning merged decorations
// The returned list is owned by the stack (cached) and valid until next render
const DecorationList *layer_stack_render(LayerStack *stack, Buffer *buffer,
                                          size_t viewport_start, size_t viewport_end);

// Invalidate the render cache (call when layers change)
void layer_stack_invalidate(LayerStack *stack);

// Notify all layers of a document edit
void layer_stack_notify_edit(LayerStack *stack, Buffer *buffer,
                              size_t edit_pos, size_t old_len, size_t new_len);

// === Action Resolution ===

// Find the action script for an interaction at a position
// Returns the script to execute (not owned, points into decoration), or NULL if none
// If out_decor is not NULL, sets it to point to the decoration with the action
const char *layer_stack_resolve_action(const LayerStack *stack, size_t pos,
                                        InteractType type,
                                        const Decoration **out_decor);

// === Style Resolution ===

// Resolved style at a position (merged from all layers)
typedef struct {
    SDL_Color fg;
    SDL_Color bg;
    bool has_fg;
    bool has_bg;
    bool underline;
} ResolvedStyle;

// Get merged style at a position from cached decorations
// Call layer_stack_render first to ensure cache is populated
void layer_stack_resolve_style(const LayerStack *stack, size_t pos,
                                ResolvedStyle *out);

// Get all decorations at a position (for inspection)
// Returns count of decorations, fills out_decors with pointers
size_t layer_stack_decorations_at(const LayerStack *stack, size_t pos,
                                   const Decoration **out_decors, size_t max_decors);

#endif // WEFT_LAYER_STACK_H
