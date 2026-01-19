#include "layer_stack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"

#define LAYER_STACK_INITIAL_CAPACITY 8

// === LayerStack Lifecycle ===

LayerStack *layer_stack_new(void) {
    LayerStack *stack = calloc(1, sizeof(LayerStack));
    if (!stack) {
        return NULL;
    }

    decoration_list_init(&stack->cache);
    stack->cache_valid = false;

    return stack;
}

void layer_stack_free(LayerStack *stack) {
    if (!stack) {
        return;
    }

    decoration_list_free(&stack->cache);
    free(stack->layers);
    free(stack);
}

// === Layer Management ===

static bool layer_stack_grow(LayerStack *stack) {
    size_t new_cap = stack->capacity == 0
                         ? LAYER_STACK_INITIAL_CAPACITY
                         : stack->capacity * 2;

    ViewLayer **new_layers = realloc(stack->layers, new_cap * sizeof(ViewLayer *));
    if (!new_layers) {
        return false;
    }

    stack->layers = new_layers;
    stack->capacity = new_cap;
    return true;
}

// Compare layers by priority for sorting
static int compare_layer_priority(const void *a, const void *b) {
    const ViewLayer *la = *(const ViewLayer **)a;
    const ViewLayer *lb = *(const ViewLayer **)b;
    return la->priority - lb->priority;  // Lower priority first
}

bool layer_stack_add(LayerStack *stack, ViewLayer *layer) {
    if (!stack || !layer) {
        return false;
    }

    // Check if already present
    for (size_t i = 0; i < stack->count; i++) {
        if (stack->layers[i] == layer) {
            return true;  // Already added
        }
    }

    if (stack->count >= stack->capacity) {
        if (!layer_stack_grow(stack)) {
            return false;
        }
    }

    stack->layers[stack->count++] = layer;

    // Keep sorted by priority
    qsort(stack->layers, stack->count, sizeof(ViewLayer *), compare_layer_priority);

    // Invalidate cache since layer order may have changed
    stack->cache_valid = false;

    return true;
}

bool layer_stack_remove(LayerStack *stack, ViewLayer *layer) {
    if (!stack || !layer) {
        return false;
    }

    for (size_t i = 0; i < stack->count; i++) {
        if (stack->layers[i] == layer) {
            // Shift remaining layers down
            memmove(&stack->layers[i], &stack->layers[i + 1],
                    (stack->count - i - 1) * sizeof(ViewLayer *));
            stack->count--;
            stack->cache_valid = false;
            return true;
        }
    }

    return false;  // Not found
}

ViewLayer *layer_stack_find(const LayerStack *stack, const char *name) {
    if (!stack || !name) {
        return NULL;
    }

    for (size_t i = 0; i < stack->count; i++) {
        if (strcmp(stack->layers[i]->name, name) == 0) {
            return stack->layers[i];
        }
    }

    return NULL;
}

size_t layer_stack_count(const LayerStack *stack) {
    return stack ? stack->count : 0;
}

ViewLayer *layer_stack_get(const LayerStack *stack, size_t index) {
    if (!stack || index >= stack->count) {
        return NULL;
    }
    return stack->layers[index];
}

// === Rendering ===

const DecorationList *layer_stack_render(LayerStack *stack, Buffer *buffer,
                                          size_t viewport_start, size_t viewport_end) {
    if (!stack || !buffer) {
        return NULL;
    }

    uint64_t doc_rev = buffer_get_doc_rev(buffer);

    // Check if cache is still valid
    if (stack->cache_valid &&
        stack->cache_doc_rev == doc_rev &&
        stack->cache_start == viewport_start &&
        stack->cache_end == viewport_end) {
        return &stack->cache;
    }

    // Clear and rebuild cache
    decoration_list_clear(&stack->cache);

    // Render each layer in priority order (lower priority first)
    for (size_t i = 0; i < stack->count; i++) {
        view_layer_render(stack->layers[i], buffer, viewport_start, viewport_end,
                          &stack->cache);
    }

    // Update cache metadata
    stack->cache_doc_rev = doc_rev;
    stack->cache_start = viewport_start;
    stack->cache_end = viewport_end;
    stack->cache_valid = true;

    return &stack->cache;
}

void layer_stack_invalidate(LayerStack *stack) {
    if (stack) {
        stack->cache_valid = false;
    }
}

void layer_stack_notify_edit(LayerStack *stack, Buffer *buffer,
                              size_t edit_pos, size_t old_len, size_t new_len) {
    if (!stack) {
        return;
    }

    // Invalidate cache
    stack->cache_valid = false;

    // Notify all layers
    for (size_t i = 0; i < stack->count; i++) {
        view_layer_notify_edit(stack->layers[i], buffer, edit_pos, old_len, new_len);
    }
}

// === Action Resolution ===

const char *layer_stack_resolve_action(const LayerStack *stack, size_t pos,
                                        InteractType type,
                                        const Decoration **out_decor) {
    if (!stack || !stack->cache_valid) {
        if (out_decor) {
            *out_decor = NULL;
        }
        return NULL;
    }

    // Search in reverse order (higher priority layers win)
    for (size_t i = stack->cache.count; i > 0; i--) {
        const Decoration *d = &stack->cache.items[i - 1];

        // Check if position is within decoration span
        if (pos >= d->start && pos < d->end) {
            // Check if decoration has an action for this event type
            if (d->action_script && d->action_type == type) {
                if (out_decor) {
                    *out_decor = d;
                }
                return d->action_script;
            }
        }
    }

    if (out_decor) {
        *out_decor = NULL;
    }
    return NULL;
}

// === Style Resolution ===

void layer_stack_resolve_style(const LayerStack *stack, size_t pos,
                                ResolvedStyle *out) {
    memset(out, 0, sizeof(ResolvedStyle));

    if (!stack || !stack->cache_valid) {
        return;
    }

    // Iterate in order (lower priority first), later layers override
    for (size_t i = 0; i < stack->cache.count; i++) {
        const Decoration *d = &stack->cache.items[i];

        // Check if position is within decoration span
        if (pos >= d->start && pos < d->end) {
            if (d->has_fg) {
                out->fg = d->fg;
                out->has_fg = true;
            }
            if (d->has_bg) {
                out->bg = d->bg;
                out->has_bg = true;
            }
            if (d->underline) {
                out->underline = true;
            }
        }
    }
}

size_t layer_stack_decorations_at(const LayerStack *stack, size_t pos,
                                   const Decoration **out_decors, size_t max_decors) {
    if (!stack || !stack->cache_valid || !out_decors) {
        return 0;
    }

    size_t found = 0;

    for (size_t i = 0; i < stack->cache.count && found < max_decors; i++) {
        const Decoration *d = &stack->cache.items[i];

        if (pos >= d->start && pos < d->end) {
            out_decors[found++] = d;
        }
    }

    return found;
}
