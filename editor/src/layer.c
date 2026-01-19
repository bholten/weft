#include "layer.h"

#include <stdlib.h>
#include <string.h>

// === ViewLayer Lifecycle ===

ViewLayer *view_layer_new(const char *name) {
  ViewLayer *layer = calloc(1, sizeof(ViewLayer));
  if (!layer) {
    return NULL;
  }

  layer->name = strdup(name);
  if (!layer->name) {
    free(layer);
    return NULL;
  }

  layer->priority = 0;
  layer->render = NULL;
  layer->update = NULL;
  layer->cleanup = NULL;
  layer->user_data = NULL;
  layer->last_doc_rev = 0;
  layer->needs_rerender = true; // Start dirty

  return layer;
}

void view_layer_free(ViewLayer *layer) {
  if (!layer) {
    return;
  }

  if (layer->cleanup) {
    layer->cleanup(layer);
  }

  free(layer->name);
  free(layer);
}

void view_layer_set_render(ViewLayer *layer, ViewLayerRenderFn render) {
  layer->render = render;
}

void view_layer_set_update(ViewLayer *layer, ViewLayerUpdateFn update) {
  layer->update = update;
}

void view_layer_set_cleanup(ViewLayer *layer, ViewLayerCleanupFn cleanup) {
  layer->cleanup = cleanup;
}

void view_layer_set_user_data(ViewLayer *layer, void *data) {
  layer->user_data = data;
}

// === ViewLayer Operations ===

void view_layer_render(ViewLayer *layer, Buffer *buffer, size_t viewport_start,
                       size_t viewport_end, DecorationList *out) {
  if (!layer || !layer->render) {
    return;
  }

  // Mark layer name on all decorations we're about to add
  size_t start_count = out->count;

  layer->render(layer, buffer, viewport_start, viewport_end, out);

  // Tag all new decorations with this layer's name
  for (size_t i = start_count; i < out->count; i++) {
    out->items[i].layer_name = layer->name;
  }

  layer->needs_rerender = false;
}

bool view_layer_notify_edit(ViewLayer *layer, Buffer *buffer, size_t edit_pos,
                            size_t old_len, size_t new_len) {
  if (!layer) {
    return false;
  }

  // Always mark as needing rerender when document changes
  layer->needs_rerender = true;

  if (layer->update) {
    return layer->update(layer, buffer, edit_pos, old_len, new_len);
  }

  // No update callback, assume full rerender needed
  return true;
}

void view_layer_invalidate(ViewLayer *layer) {
  if (layer) {
    layer->needs_rerender = true;
  }
}

bool view_layer_needs_render(const ViewLayer *layer, uint64_t current_doc_rev) {
  if (!layer) {
    return false;
  }

  return layer->needs_rerender || layer->last_doc_rev != current_doc_rev;
}
