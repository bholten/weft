#include "plumb_layer.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"
#include "document.h"

// Helper: Check if character can be part of a filename
static bool is_filename_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
}

// Helper: Check if character is a digit
static bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

// Scan a region of text for file:line patterns and emit decorations
static void scan_for_patterns(const char *text, size_t text_len,
                              size_t base_offset, DecorationList *out) {
  size_t i = 0;

  while (i < text_len) {
    // Skip non-filename characters
    while (i < text_len && !is_filename_char(text[i])) {
      i++;
    }
    if (i >= text_len) {
      break;
    }

    size_t file_start = i;

    // Scan filename (must contain at least one '.' for extension)
    bool has_dot = false;
    while (i < text_len && is_filename_char(text[i])) {
      if (text[i] == '.') {
        has_dot = true;
      }
      i++;
    }
    size_t file_end = i;

    // Check for :number pattern
    if (i < text_len && text[i] == ':' && has_dot) {
      i++;

      // Must have at least one digit
      if (i < text_len && is_digit(text[i])) {
        size_t line_start = i;
        while (i < text_len && is_digit(text[i])) {
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
          if (i < text_len && text[i] == ':' && i + 1 < text_len &&
              is_digit(text[i + 1])) {
            i++; // skip :
            while (i < text_len && is_digit(text[i])) {
              i++; // skip column digits
            }
          }

          // Extract filename
          size_t fname_len = file_end - file_start;
          char *filename = malloc(fname_len + 1);
          if (filename) {
            memcpy(filename, text + file_start, fname_len);
            filename[fname_len] = '\0';

            // Create handler script
            char *handler = malloc(PATH_MAX + 256);
            if (handler) {
              snprintf(
                  handler, PATH_MAX + 256,
                  "weft::buffer::open-at \"%s\" %d\nweft::interact::handled",
                  filename, line_num);

              // Create decoration for the file:line span
              size_t span_start = base_offset + file_start;
              size_t span_end = base_offset + line_end;

              Decoration *d = decoration_list_add(out, span_start, span_end);
              if (d) {
                // Style: underline to indicate clickable
                d->underline = true;

                // Set up middle-click action
                decoration_set_action(d, INTERACT_MIDDLE_CLICK, handler);

                // Add metadata for inspection
                decoration_add_meta(d, "plumb:file", filename);
                decoration_add_meta(d, "plumb:line", line_str);
              } else {
                free(handler);
              }
            }

            free(filename);
          }
        }
      }
    }
  }
}

// Layer render callback
static void plumb_layer_render(ViewLayer *layer, Buffer *buffer,
                               size_t viewport_start, size_t viewport_end,
                               DecorationList *out) {
  (void)layer;

  if (!buffer || !buffer->document) {
    return;
  }

  Document *doc = buffer->document;
  size_t doc_len = document_length(doc);

  if (doc_len == 0 || viewport_start >= doc_len) {
    return;
  }

  // Clamp viewport to document bounds
  if (viewport_end > doc_len) {
    viewport_end = doc_len;
  }

  size_t region_len = viewport_end - viewport_start;
  if (region_len == 0) {
    return;
  }

  // Read the viewport region
  char *text = malloc(region_len + 1);
  if (!text) {
    return;
  }

  size_t got = document_get_text(doc, viewport_start, text, region_len);
  text[got] = '\0';

  // Scan for patterns and emit decorations
  scan_for_patterns(text, got, viewport_start, out);

  free(text);
}

// === Public API ===

ViewLayer *plumb_layer_new(int priority) {
  ViewLayer *layer = view_layer_new("plumb");
  if (!layer) {
    return NULL;
  }

  layer->priority = priority;
  view_layer_set_render(layer, plumb_layer_render);
  // No cleanup needed - no user_data

  return layer;
}
