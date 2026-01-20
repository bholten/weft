#include "annot_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "annot_store.h"
#include "buffer.h"
#include "color.h"

// Layer user data
typedef struct {
  char *annot_layer_name; // Which annotation layer to read from
} AnnotLayerData;

// === Layer callbacks ===

static void annot_layer_render(ViewLayer *layer, Buffer *buffer,
                               size_t viewport_start, size_t viewport_end,
                               DecorationList *out) {
  if (!layer || !buffer || !buffer->annots) {
    return;
  }

  AnnotLayerData *data = (AnnotLayerData *)layer->user_data;
  if (!data) {
    return;
  }

  // Query annotations in the viewport range
  AnnotQuery query;
  annot_query_init(&query);
  annot_store_in_range(buffer->annots, viewport_start, viewport_end,
                       data->annot_layer_name, &query);

  // Convert each annotation to a decoration
  for (size_t i = 0; i < query.count; i++) {
    uint32_t annot_id = query.ids[i];

    // Get annotation span
    size_t start, end;
    if (!annot_store_get_span(buffer->annots, annot_id, &start, &end)) {
      continue;
    }

    // Create decoration
    Decoration *d = decoration_list_add(out, start, end);
    if (!d) {
      continue;
    }

    // Parse style metadata
    const char *fg = annot_store_get_meta(buffer->annots, annot_id, "style:fg");
    if (fg && color_parse(fg, &d->fg)) {
      d->has_fg = true;
    }

    const char *bg = annot_store_get_meta(buffer->annots, annot_id, "style:bg");
    if (bg && color_parse(bg, &d->bg)) {
      d->has_bg = true;
    }

    const char *ul =
        annot_store_get_meta(buffer->annots, annot_id, "style:underline");
    if (ul && (strcmp(ul, "1") == 0 || strcmp(ul, "true") == 0)) {
      d->underline = true;
    }

    // Check for action handlers
    const char *on_middle = annot_store_get_meta(buffer->annots, annot_id,
                                                 "action:on_middle_click");
    if (on_middle) {
      decoration_set_action(d, INTERACT_MIDDLE_CLICK, strdup(on_middle));
    }

    const char *on_left =
        annot_store_get_meta(buffer->annots, annot_id, "action:on_left_click");
    if (on_left) {
      decoration_set_action(d, INTERACT_LEFT_CLICK, strdup(on_left));
    }

    const char *on_right =
        annot_store_get_meta(buffer->annots, annot_id, "action:on_right_click");
    if (on_right) {
      decoration_set_action(d, INTERACT_RIGHT_CLICK, strdup(on_right));
    }

    // Copy relevant metadata for inspection
    const AnnotRecord *record = annot_store_get(buffer->annots, annot_id);
    if (record) {
      for (size_t j = 0; j < record->meta_count; j++) {
        // Skip style: and action: prefixes (already handled)
        if (strncmp(record->keys[j], "style:", 6) == 0 ||
            strncmp(record->keys[j], "action:", 7) == 0) {
          continue;
        }
        decoration_add_meta(d, record->keys[j], record->values[j]);
      }
    }
  }

  annot_query_free(&query);
}

static void annot_layer_cleanup(ViewLayer *layer) {
  if (!layer || !layer->user_data) {
    return;
  }

  AnnotLayerData *data = (AnnotLayerData *)layer->user_data;
  free(data->annot_layer_name);
  free(data);
  layer->user_data = NULL;
}

// === Public API ===

ViewLayer *annot_layer_new(const char *annot_layer_name, int priority) {
  ViewLayer *layer = view_layer_new(annot_layer_name);
  if (!layer) {
    return NULL;
  }

  AnnotLayerData *data = calloc(1, sizeof(AnnotLayerData));
  if (!data) {
    view_layer_free(layer);
    return NULL;
  }

  data->annot_layer_name = strdup(annot_layer_name);
  if (!data->annot_layer_name) {
    free(data);
    view_layer_free(layer);
    return NULL;
  }

  layer->priority = priority;
  view_layer_set_render(layer, annot_layer_render);
  view_layer_set_cleanup(layer, annot_layer_cleanup);
  view_layer_set_user_data(layer, data);

  return layer;
}
