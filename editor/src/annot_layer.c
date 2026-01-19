#include "annot_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // For strcasecmp

#include "annot_store.h"
#include "buffer.h"

// Layer user data
typedef struct {
  char *annot_layer_name; // Which annotation layer to read from
} AnnotLayerData;

// === Color parsing (duplicated from pane.c for now, will be unified later) ===

static bool parse_hex_color(const char *str, SDL_Color *out) {
  if (!str || str[0] != '#') {
    return false;
  }

  size_t len = strlen(str);
  unsigned int r, g, b;

  if (len == 7) {
    if (sscanf(str, "#%02x%02x%02x", &r, &g, &b) == 3) {
      out->r = (uint8_t)r;
      out->g = (uint8_t)g;
      out->b = (uint8_t)b;
      out->a = 255;
      return true;
    }
  } else if (len == 4) {
    if (sscanf(str, "#%1x%1x%1x", &r, &g, &b) == 3) {
      out->r = (uint8_t)(r * 17);
      out->g = (uint8_t)(g * 17);
      out->b = (uint8_t)(b * 17);
      out->a = 255;
      return true;
    }
  }

  return false;
}

static bool parse_named_color(const char *str, SDL_Color *out) {
  if (!str) {
    return false;
  }

  struct {
    const char *name;
    uint8_t r, g, b;
  } colors[] = {
      {"red",     255, 0,   0  },
      {"green",   0,   255, 0  },
      {"blue",    0,   0,   255},
      {"yellow",  255, 255, 0  },
      {"cyan",    0,   255, 255},
      {"magenta", 255, 0,   255},
      {"white",   255, 255, 255},
      {"black",   0,   0,   0  },
      {"orange",  255, 165, 0  },
      {"purple",  128, 0,   128},
      {"pink",    255, 192, 203},
      {"gray",    128, 128, 128},
      {"grey",    128, 128, 128},
  };

  for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
    if (strcasecmp(str, colors[i].name) == 0) {
      out->r = colors[i].r;
      out->g = colors[i].g;
      out->b = colors[i].b;
      out->a = 255;
      return true;
    }
  }

  return false;
}

static bool parse_color(const char *str, SDL_Color *out) {
  return parse_hex_color(str, out) || parse_named_color(str, out);
}

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
    if (fg && parse_color(fg, &d->fg)) {
      d->has_fg = true;
    }

    const char *bg = annot_store_get_meta(buffer->annots, annot_id, "style:bg");
    if (bg && parse_color(bg, &d->bg)) {
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
