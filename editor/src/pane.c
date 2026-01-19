#include "pane.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "annot_layer.h"
#include "annot_store.h"
#include "document.h"
#include "plumb_layer.h"
#include "renderer.h"
#include "tree_sitter_layer.h"

#define TEXT_PADDING 10
#define MAX_STYLED_SPANS 64

// Text style from annotations
typedef struct {
  SDL_Color fg;
  SDL_Color bg;
  bool has_fg;
  bool has_bg;
  bool underline;
} TextStyle;

// Styled span for rendering
typedef struct {
  size_t start; // Byte offset in document
  size_t end;   // Byte offset (exclusive)
  TextStyle style;
} StyledSpan;

// Parse hex color string like "#RRGGBB" or "#RGB"
static bool parse_hex_color(const char *str, SDL_Color *out) {
  if (!str || str[0] != '#') {
    return false;
  }

  size_t len = strlen(str);
  unsigned int r, g, b;

  if (len == 7) {
    // #RRGGBB
    if (sscanf(str, "#%02x%02x%02x", &r, &g, &b) == 3) {
      out->r = (uint8_t)r;
      out->g = (uint8_t)g;
      out->b = (uint8_t)b;
      out->a = 255;
      return true;
    }
  } else if (len == 4) {
    // #RGB -> #RRGGBB
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

// Parse named color
static bool parse_named_color(const char *str, SDL_Color *out) {
  if (!str) {
    return false;
  }

  // Basic named colors
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

// Parse any color value
static bool parse_color(const char *str, SDL_Color *out) {
  return parse_hex_color(str, out) || parse_named_color(str, out);
}

// Build TextStyle from annotation metadata
static void style_from_annot(const AnnotStore *store, uint32_t annot_id,
                             TextStyle *style) {
  memset(style, 0, sizeof(TextStyle));

  const char *fg = annot_store_get_meta(store, annot_id, "style:fg");
  if (fg && parse_color(fg, &style->fg)) {
    style->has_fg = true;
  }

  const char *bg = annot_store_get_meta(store, annot_id, "style:bg");
  if (bg && parse_color(bg, &style->bg)) {
    style->has_bg = true;
  }

  const char *ul = annot_store_get_meta(store, annot_id, "style:underline");
  if (ul && (strcmp(ul, "1") == 0 || strcmp(ul, "true") == 0)) {
    style->underline = true;
  }
}

// Helper to expand tabs to spaces in a line buffer for display
// Returns the visual length after tab expansion
// Also fills doc_to_visual mapping if provided (must be at least src_len
// elements)
static size_t expand_tabs(const char *src, size_t src_len, char *dst,
                          size_t dst_cap, int tab_size, size_t *doc_to_visual) {
  if (tab_size <= 0) {
    tab_size = 4;
  }

  size_t visual_col = 0;
  size_t dst_pos = 0;

  for (size_t i = 0; i < src_len && dst_pos < dst_cap - 1; i++) {
    if (doc_to_visual) {
      doc_to_visual[i] = visual_col;
    }

    if (src[i] == '\t') {
      // Calculate spaces to next tab stop
      int spaces = tab_size - (int)(visual_col % (size_t)tab_size);
      for (int j = 0; j < spaces && dst_pos < dst_cap - 1; j++) {
        dst[dst_pos++] = ' ';
        visual_col++;
      }
    } else {
      dst[dst_pos++] = src[i];
      visual_col++;
    }
  }

  dst[dst_pos] = '\0';
  return visual_col;
}

// Convert document byte offset within a line to visual column (accounting for
// tabs)
static size_t doc_offset_to_visual_col(const char *line_buf, size_t line_len,
                                       size_t doc_offset, int tab_size) {
  if (tab_size <= 0) {
    tab_size = 4;
  }

  size_t visual_col = 0;
  for (size_t i = 0; i < line_len && i < doc_offset; i++) {
    if (line_buf[i] == '\t') {
      // Advance to next tab stop
      int spaces = tab_size - (int)(visual_col % (size_t)tab_size);
      visual_col += (size_t)spaces;
    } else {
      visual_col++;
    }
  }
  return visual_col;
}

// Tag bar command strings and their positions (computed during render)
static const char *TAG_COMMANDS[] = {"New", "Open", "Save", "SaveAs", "Del"};
static const int TAG_COMMAND_COUNT = 5;

Pane *pane_new(Buffer *buffer, int char_width, int line_height) {
  if (!buffer) {
    return NULL;
  }

  Pane *p = calloc(1, sizeof(Pane));
  if (!p) {
    return NULL;
  }

  p->view = editor_new(buffer);
  if (!p->view) {
    free(p);
    return NULL;
  }

  // Create layer stack for rendering
  p->layer_stack = layer_stack_new();
  if (!p->layer_stack) {
    editor_free(p->view);
    free(p);
    return NULL;
  }

  // Add layers in priority order:
  // - style (0): syntax highlighting from annotations (only if no tree-sitter)
  // - ts:* (2): tree-sitter syntax highlighting (if file type detected)
  // - plumb (5): clickable file:line patterns
  // - search (10): search result highlights (on top)
  ViewLayer *plumb_layer = plumb_layer_new(5);
  ViewLayer *search_layer = annot_layer_new("search", 10);

  // Check if we should use tree-sitter for syntax highlighting
  bool use_tree_sitter = false;
  const char *file_path = buffer_get_file_path(buffer);
  if (file_path) {
    size_t len = strlen(file_path);
    // Check for .lcl extension or *lcl* buffer name (REPL)
    bool is_lcl = (len > 4 && strcmp(file_path + len - 4, ".lcl") == 0) ||
                  strcmp(file_path, "*lcl*") == 0;
    if (is_lcl) {
      ViewLayer *ts_layer = tree_sitter_layer_new_lcl(2);
      if (ts_layer) {
        layer_stack_add(p->layer_stack, ts_layer);
        use_tree_sitter = true;

        // Clear any existing style annotations to avoid conflicts
        if (buffer->annots) {
          annot_store_clear_layer(buffer->annots, "style");
        }
      }
    }
  }

  // Only add style annotation layer if not using tree-sitter
  // (tree-sitter computes highlighting, annotations would conflict)
  if (!use_tree_sitter) {
    ViewLayer *style_layer = annot_layer_new("style", 0);
    if (style_layer) {
      layer_stack_add(p->layer_stack, style_layer);
    }
  }

  if (plumb_layer) {
    layer_stack_add(p->layer_stack, plumb_layer);
  }
  if (search_layer) {
    layer_stack_add(p->layer_stack, search_layer);
  }

  p->char_width = char_width;
  p->line_height = line_height;
  p->text_offset_x = TEXT_PADDING;
  p->text_offset_y = TEXT_PADDING;

  // Set editor layout metrics
  editor_set_layout(p->view, char_width, line_height, p->text_offset_x,
                    p->text_offset_y);

  // Default bounds (will be set properly by layout)
  p->bounds = (SDL_Rect){.x = 0, .y = 0, .w = 800, .h = 600};
  editor_set_viewport_height(p->view,
                             p->bounds.h - TAG_BAR_HEIGHT - STATUS_BAR_HEIGHT);

  return p;
}

void pane_free(Pane *p) {
  if (p) {
    // Free layers in the stack (we own them)
    if (p->layer_stack) {
      for (size_t i = 0; i < layer_stack_count(p->layer_stack); i++) {
        ViewLayer *layer = layer_stack_get(p->layer_stack, i);
        view_layer_free(layer);
      }
      layer_stack_free(p->layer_stack);
    }
    editor_free(p->view);
    free(p->matches);
    free(p);
  }
}

// Calculate total header height (tag bar + search bar if visible)
static int pane_header_height(const Pane *p) {
  int height = TAG_BAR_HEIGHT;
  if (p->search_visible) {
    height += SEARCH_BAR_HEIGHT;
    if (p->replace_mode) {
      height += SEARCH_BAR_HEIGHT; // Extra row for replace input
    }
  }
  return height;
}

// Calculate gutter width based on line count
static int calc_gutter_width(size_t line_count, int char_width) {
  // Count digits needed
  int digits = 1;
  size_t n = line_count;
  while (n >= 10) {
    digits++;
    n /= 10;
  }
  // At least 3 digits, plus padding
  if (digits < 3) {
    digits = 3;
  }
  int width = (digits + 1) * char_width; // +1 for spacing
  return width > MIN_GUTTER_WIDTH ? width : MIN_GUTTER_WIDTH;
}

// Get current gutter width for this pane
static int pane_gutter_width(const Pane *p) {
  if (!p || !p->view) {
    return MIN_GUTTER_WIDTH;
  }
  Buffer *buf = editor_get_buffer(p->view);
  if (!buf || !buf->document) {
    return MIN_GUTTER_WIDTH;
  }
  size_t line_count = document_line_count(buf->document);
  return calc_gutter_width(line_count, p->char_width);
}

void pane_set_bounds(Pane *p, SDL_Rect bounds) {
  if (!p) {
    return;
  }
  p->bounds = bounds;
  editor_set_viewport_height(p->view, bounds.h - pane_header_height(p) -
                                          STATUS_BAR_HEIGHT);
}

SDL_Rect pane_get_bounds(const Pane *p) {
  if (!p) {
    return (SDL_Rect){0, 0, 0, 0};
  }
  return p->bounds;
}

bool pane_contains_point(const Pane *p, int x, int y) {
  if (!p) {
    return false;
  }
  return x >= p->bounds.x && x < p->bounds.x + p->bounds.w &&
         y >= p->bounds.y && y < p->bounds.y + p->bounds.h;
}

void pane_window_to_local(const Pane *p, int wx, int wy, int *lx, int *ly) {
  if (!p || !lx || !ly) {
    return;
  }
  int gutter_width = pane_gutter_width(p);
  *lx = wx - p->bounds.x - gutter_width;
  *ly = wy - p->bounds.y - pane_header_height(p); // Account for header
}

void pane_local_to_window(const Pane *p, int lx, int ly, int *wx, int *wy) {
  if (!p || !wx || !wy) {
    return;
  }
  int gutter_width = pane_gutter_width(p);
  *wx = lx + p->bounds.x + gutter_width;
  *wy = ly + p->bounds.y + pane_header_height(p); // Account for header
}

void pane_render(Pane *p, Renderer *r, bool focused) {
  if (!p || !r) {
    return;
  }

  Editor *e = p->view;
  Buffer *buf = editor_get_buffer(e);
  Document *doc = buf->document;

  int scroll_y = editor_get_scroll_y(e);
  int line_height = p->line_height;
  int char_width = p->char_width;

  // Draw pane background
  SDL_Color bg_color = {.r = 30, .g = 30, .b = 30, .a = 255};
  renderer_draw_rect(r, p->bounds.x, p->bounds.y, p->bounds.w, p->bounds.h,
                     bg_color);

  // Draw tag bar background
  SDL_Color tag_bg =
      focused
          ? (SDL_Color){.r = 70, .g = 130, .b = 180, .a = 255}
          // Blue when focused
          : (SDL_Color){
                .r = 60, .g = 60, .b = 70, .a = 255}; // Gray when not focused
  renderer_draw_rect(r, p->bounds.x, p->bounds.y, p->bounds.w, TAG_BAR_HEIGHT,
                     tag_bg);

  // Draw tag bar content
  SDL_Color tag_text = {.r = 255, .g = 255, .b = 255, .a = 255};
  int tag_x = p->bounds.x + 4;
  int tag_y = p->bounds.y + 2;

  // Draw filename (or "untitled")
  const char *filepath = buffer_get_file_path(buf);
  const char *display_name = filepath ? filepath : "[untitled]";

  // If path is long, just show the filename part
  if (filepath) {
    const char *last_slash = strrchr(filepath, '/');
    if (last_slash) {
      display_name = last_slash + 1;
    }
  }

  // Add modified indicator
  char name_buf[128];
  if (buffer_is_modified(buf)) {
    snprintf(name_buf, sizeof(name_buf), "*%s", display_name);
    display_name = name_buf;
  }

  renderer_draw_text(r, display_name, tag_x, tag_y, tag_text);

  // Draw commands on the right side
  int cmd_x = p->bounds.x + p->bounds.w - 4;
  for (int i = TAG_COMMAND_COUNT - 1; i >= 0; i--) {
    int cmd_len = (int)strlen(TAG_COMMANDS[i]);
    cmd_x -= cmd_len * char_width + 8; // 8px spacing between commands
    renderer_draw_text(r, TAG_COMMANDS[i], cmd_x, tag_y, tag_text);
  }

  // Draw search bar if visible
  if (p->search_visible) {
    int search_y = p->bounds.y + TAG_BAR_HEIGHT;
    SDL_Color search_bg = {.r = 50, .g = 50, .b = 55, .a = 255};
    SDL_Color input_bg = {.r = 40, .g = 40, .b = 45, .a = 255};
    SDL_Color input_text = {.r = 255, .g = 255, .b = 255, .a = 255};
    SDL_Color label_text = {.r = 180, .g = 180, .b = 180, .a = 255};
    SDL_Color match_text = {.r = 150, .g = 150, .b = 150, .a = 255};

    // Search row background
    renderer_draw_rect(r, p->bounds.x, search_y, p->bounds.w, SEARCH_BAR_HEIGHT,
                       search_bg);

    // "Find:" label
    int label_x = p->bounds.x + 4;
    int input_y = search_y + 4;
    renderer_draw_text(r, "Find:", label_x, input_y, label_text);

    // Search input box
    int input_x = label_x + 5 * char_width + 4;
    int input_w = 200;
    renderer_draw_rect(r, input_x, search_y + 2, input_w, SEARCH_BAR_HEIGHT - 4,
                       input_bg);
    renderer_draw_text(r, p->search_query, input_x + 2, input_y, input_text);

    // Draw cursor in search input if focused
    if (focused && p->search_focused) {
      int cursor_x = input_x + 2 + (int)p->search_cursor * char_width;
      renderer_draw_rect(r, cursor_x, search_y + 4, 2, SEARCH_BAR_HEIGHT - 8,
                         input_text);
    }

    // Match count
    char match_info[32];
    if (p->match_count > 0) {
      snprintf(match_info, sizeof(match_info), "%zu/%zu",
               p->current_match != SIZE_MAX ? p->current_match + 1 : 0,
               p->match_count);
    } else if (p->search_query[0] != '\0') {
      snprintf(match_info, sizeof(match_info), "0 matches");
    } else {
      match_info[0] = '\0';
    }
    if (match_info[0]) {
      int info_x = input_x + input_w + 8;
      renderer_draw_text(r, match_info, info_x, input_y, match_text);
    }

    // Replace row if in replace mode
    if (p->replace_mode) {
      int replace_y = search_y + SEARCH_BAR_HEIGHT;
      renderer_draw_rect(r, p->bounds.x, replace_y, p->bounds.w,
                         SEARCH_BAR_HEIGHT, search_bg);

      // "Replace:" label
      renderer_draw_text(r, "Replace:", label_x, replace_y + 4, label_text);

      // Replace input box
      int repl_input_x = label_x + 8 * char_width + 4;
      renderer_draw_rect(r, repl_input_x, replace_y + 2, input_w,
                         SEARCH_BAR_HEIGHT - 4, input_bg);
      renderer_draw_text(r, p->replace_query, repl_input_x + 2, replace_y + 4,
                         input_text);

      // Draw cursor in replace input if focused
      if (focused && !p->search_focused) {
        int cursor_x = repl_input_x + 2 + (int)p->replace_cursor * char_width;
        renderer_draw_rect(r, cursor_x, replace_y + 4, 2, SEARCH_BAR_HEIGHT - 8,
                           input_text);
      }

      // Replace buttons
      int btn_x = repl_input_x + input_w + 8;
      renderer_draw_text(r, "[Replace]", btn_x, replace_y + 4, label_text);
      btn_x += 10 * char_width;
      renderer_draw_text(r, "[All]", btn_x, replace_y + 4, label_text);
    }
  }

  int header_height = pane_header_height(p);
  size_t line_count = document_line_count(doc);

  // Draw focus indicator (border) - but not on top where header is
  if (focused) {
    SDL_Color focus_color = {.r = 70, .g = 130, .b = 180, .a = 255};
    // Bottom border (above status bar)
    renderer_draw_rect(r, p->bounds.x,
                       p->bounds.y + p->bounds.h - STATUS_BAR_HEIGHT - 2,
                       p->bounds.w, 2, focus_color);
    // Left border (below header, above status bar)
    renderer_draw_rect(r, p->bounds.x, p->bounds.y + header_height, 2,
                       p->bounds.h - header_height - STATUS_BAR_HEIGHT,
                       focus_color);
    // Right border (below header, above status bar)
    renderer_draw_rect(
        r, p->bounds.x + p->bounds.w - 2, p->bounds.y + header_height, 2,
        p->bounds.h - header_height - STATUS_BAR_HEIGHT, focus_color);
  }

  // Draw status bar at bottom
  int status_y = p->bounds.y + p->bounds.h - STATUS_BAR_HEIGHT;
  SDL_Color status_bg = {.r = 45, .g = 45, .b = 50, .a = 255};
  SDL_Color status_text = {.r = 180, .g = 180, .b = 180, .a = 255};
  renderer_draw_rect(r, p->bounds.x, status_y, p->bounds.w, STATUS_BAR_HEIGHT,
                     status_bg);

  // Status bar content: line:column and cursor position
  size_t cursor_pos = editor_get_cursor_pos(e);
  size_t cursor_line = document_pos_to_line(doc, cursor_pos);
  size_t line_start_pos = document_line_start(doc, cursor_line);
  size_t cursor_col = cursor_pos - line_start_pos;

  char status_buf[128];
  snprintf(status_buf, sizeof(status_buf), "Ln %zu, Col %zu    %zu lines",
           cursor_line + 1, cursor_col + 1, line_count);
  renderer_draw_text(r, status_buf, p->bounds.x + 8, status_y + 2, status_text);

  // Calculate gutter and text area
  int gutter_width = calc_gutter_width(line_count, char_width);
  int gutter_x = p->bounds.x;
  int text_x = p->bounds.x + gutter_width + p->text_offset_x;
  int text_y = p->bounds.y + header_height + p->text_offset_y;
  int text_area_height = p->bounds.h - header_height - STATUS_BAR_HEIGHT;

  // Draw gutter background
  SDL_Color gutter_bg = {.r = 35, .g = 35, .b = 40, .a = 255};
  renderer_draw_rect(r, gutter_x, p->bounds.y + header_height, gutter_width,
                     text_area_height, gutter_bg);

  // Get tab size from editor
  int tab_size = e->tab_size > 0 ? e->tab_size : 4;

  // Draw selection highlight
  if (editor_has_selection(e)) {
    size_t sel_start, sel_end;
    editor_get_selection(e, &sel_start, &sel_end);

    size_t start_line = document_pos_to_line(doc, sel_start);
    size_t end_line = document_pos_to_line(doc, sel_end);

    SDL_Color sel_color = {.r = 70, .g = 130, .b = 180, .a = 180};

    for (size_t line = start_line; line <= end_line; line++) {
      size_t line_start_pos = document_line_start(doc, line);
      size_t line_end_pos = document_line_end(doc, line);

      // Get line text for tab-aware column calculation
      char sel_line_buf[256];
      size_t line_len = line_end_pos - line_start_pos;
      if (line_len > sizeof(sel_line_buf) - 1) {
        line_len = sizeof(sel_line_buf) - 1;
      }
      document_get_text(doc, line_start_pos, sel_line_buf, line_len);

      // Determine highlight start/end within this line
      size_t highlight_start =
          (sel_start > line_start_pos) ? sel_start : line_start_pos;
      size_t highlight_end = (sel_end < line_end_pos) ? sel_end : line_end_pos;

      if (highlight_start < highlight_end) {
        // Convert document offsets to visual columns (tab-aware)
        size_t start_offset = highlight_start - line_start_pos;
        size_t end_offset = highlight_end - line_start_pos;
        int start_col = (int)doc_offset_to_visual_col(sel_line_buf, line_len,
                                                      start_offset, tab_size);
        int end_col = (int)doc_offset_to_visual_col(sel_line_buf, line_len,
                                                    end_offset, tab_size);

        int x = text_x + start_col * char_width;
        int y = text_y + (int)line * line_height - scroll_y;
        int width = (end_col - start_col) * char_width;

        // Clip to pane bounds
        if (y >= p->bounds.y && y < p->bounds.y + p->bounds.h) {
          renderer_draw_rect(r, x, y, width, line_height, sel_color);
        }
      }
    }
  }

  // Draw document content line by line with annotation styling
  SDL_Color default_fg = {.r = 255, .g = 255, .b = 255, .a = 255};
  int y = text_y - scroll_y;
  char line_buf[256];

  // Calculate visible document range
  size_t first_vis_line = (scroll_y > 0) ? (size_t)(scroll_y / line_height) : 0;
  if (first_vis_line > 0 && first_vis_line > 0) {
    first_vis_line--; // Include one line before for partial visibility
  }
  size_t last_vis_line =
      first_vis_line + (size_t)(p->bounds.h / line_height) + 2;
  if (last_vis_line >= line_count) {
    last_vis_line = line_count > 0 ? line_count - 1 : 0;
  }

  size_t vis_start = document_line_start(doc, first_vis_line);
  size_t vis_end = document_line_end(doc, last_vis_line);

  // Render all layers for the visible range
  const DecorationList *decorations =
      layer_stack_render(p->layer_stack, buf, vis_start, vis_end);

  // Line number colors
  SDL_Color linenum_color = {.r = 100, .g = 100, .b = 110, .a = 255};
  SDL_Color linenum_current = {.r = 180, .g = 180, .b = 180, .a = 255};
  size_t current_line = document_pos_to_line(doc, editor_get_cursor_pos(e));

  for (size_t i = 0; i < line_count; i++) {
    // Skip lines that are off-screen (optimization)
    int line_bottom = y + line_height;
    if (line_bottom < p->bounds.y + header_height) {
      y += line_height;
      continue;
    }
    if (y > p->bounds.y + p->bounds.h - STATUS_BAR_HEIGHT) {
      break;
    }

    // Draw line number
    char linenum_buf[16];
    snprintf(linenum_buf, sizeof(linenum_buf), "%zu", i + 1);
    int linenum_x =
        gutter_x + gutter_width - (int)strlen(linenum_buf) * char_width - 4;
    SDL_Color ln_color = (i == current_line) ? linenum_current : linenum_color;
    if (y >= p->bounds.y + header_height) {
      renderer_draw_text(r, linenum_buf, linenum_x, y, ln_color);
    }

    size_t line_start = document_line_start(doc, i);
    size_t line_end = document_line_end(doc, i);
    size_t line_len = line_end - line_start;

    if (line_len > sizeof(line_buf) - 1) {
      line_len = sizeof(line_buf) - 1;
    }

    size_t got = document_get_text(doc, line_start, line_buf, line_len);
    line_buf[got] = '\0';

    // Expand tabs for display
    char expanded_buf[512];    // Larger buffer for expanded tabs
    size_t doc_to_visual[256]; // Map document position to visual position
    size_t visual_len =
        expand_tabs(line_buf, got, expanded_buf, sizeof(expanded_buf), tab_size,
                    doc_to_visual);
    (void)visual_len; // Used implicitly in rendering

    // Collect styled spans for this line from decorations
    StyledSpan spans[MAX_STYLED_SPANS];
    size_t span_count = 0;

    // Find decorations that overlap this line
    if (decorations) {
      for (size_t di = 0;
           di < decorations->count && span_count < MAX_STYLED_SPANS; di++) {
        const Decoration *d = &decorations->items[di];

        // Check overlap with this line
        if (d->start >= line_end || d->end <= line_start) {
          continue;
        }

        // Clamp to line bounds
        size_t span_start = (d->start > line_start) ? d->start : line_start;
        size_t span_end = (d->end < line_end) ? d->end : line_end;

        if (span_end - span_start > line_len) {
          span_end = line_start + line_len;
        }

        spans[span_count].start = span_start;
        spans[span_count].end = span_end;

        // Copy style from decoration
        spans[span_count].style.fg = d->fg;
        spans[span_count].style.bg = d->bg;
        spans[span_count].style.has_fg = d->has_fg;
        spans[span_count].style.has_bg = d->has_bg;
        spans[span_count].style.underline = d->underline;

        span_count++;
      }
    }

    if (span_count == 0) {
      // No annotations on this line - render with expanded tabs
      renderer_draw_text(r, expanded_buf, text_x, y, default_fg);
    } else {
      // Render line in segments
      size_t pos = line_start;
      int x = text_x;

      while (pos < line_start + got) {
        // Merge styles from ALL overlapping spans at this position
        TextStyle merged = {0};
        size_t seg_end = line_start + got;
        bool has_any_style = false;

        for (size_t si = 0; si < span_count; si++) {
          if (spans[si].start <= pos && spans[si].end > pos) {
            // This span covers current position - merge its style
            has_any_style = true;
            TextStyle *s = &spans[si].style;

            // Later spans override earlier ones (search overrides style)
            if (s->has_fg) {
              merged.fg = s->fg;
              merged.has_fg = true;
            }
            if (s->has_bg) {
              merged.bg = s->bg;
              merged.has_bg = true;
            }
            if (s->underline) {
              merged.underline = true;
            }

            // Segment ends at the earliest span end
            if (spans[si].end < seg_end) {
              seg_end = spans[si].end;
            }
          } else if (spans[si].start > pos && spans[si].start < seg_end) {
            // This span starts after pos - it limits our segment
            seg_end = spans[si].start;
          }
        }

        // Clamp to line end
        if (seg_end > line_start + got) {
          seg_end = line_start + got;
        }

        size_t seg_len = seg_end - pos;
        size_t buf_offset = pos - line_start;

        if (seg_len > 0 && buf_offset < got) {
          // Calculate visual positions (tab-aware)
          size_t vis_start = doc_to_visual[buf_offset];
          size_t vis_end =
              (buf_offset + seg_len < got)
                  ? doc_to_visual[buf_offset + seg_len]
                  : doc_offset_to_visual_col(line_buf, got,
                                             buf_offset + seg_len, tab_size);
          int seg_width = (int)(vis_end - vis_start) * char_width;

          // Draw background if styled
          if (merged.has_bg) {
            renderer_draw_rect(r, x, y, seg_width, line_height, merged.bg);
          }

          // Draw text (using expanded buffer with visual positions)
          SDL_Color fg = merged.has_fg ? merged.fg : default_fg;
          size_t exp_len = vis_end - vis_start;
          renderer_draw_text_n(r, expanded_buf + vis_start, exp_len, x, y, fg);

          // Draw underline if styled
          if (merged.underline) {
            int ul_y = y + line_height - 2;
            renderer_draw_line(r, x, ul_y, x + seg_width, ul_y, fg);
          }

          x += seg_width;
        }

        pos = seg_end;
      }
    }

    y += line_height;
  }

  // Draw goto line dialog if visible
  if (p->goto_visible) {
    // Dialog dimensions
    int dialog_w = 200;
    int dialog_h = 60;
    int dialog_x = p->bounds.x + (p->bounds.w - dialog_w) / 2;
    int dialog_y = p->bounds.y + header_height + 40;

    // Dialog background with border
    SDL_Color dialog_bg = {.r = 45, .g = 45, .b = 50, .a = 255};
    SDL_Color dialog_border = {.r = 70, .g = 130, .b = 180, .a = 255};
    SDL_Color label_text = {.r = 180, .g = 180, .b = 180, .a = 255};
    SDL_Color input_bg = {.r = 35, .g = 35, .b = 40, .a = 255};
    SDL_Color input_text = {.r = 255, .g = 255, .b = 255, .a = 255};

    // Draw border (slightly larger)
    renderer_draw_rect(r, dialog_x - 2, dialog_y - 2, dialog_w + 4,
                       dialog_h + 4, dialog_border);
    // Draw background
    renderer_draw_rect(r, dialog_x, dialog_y, dialog_w, dialog_h, dialog_bg);

    // Draw label
    renderer_draw_text(r, "Go to line:", dialog_x + 10, dialog_y + 8,
                       label_text);

    // Draw input box
    int input_x = dialog_x + 10;
    int input_y = dialog_y + 28;
    int input_w = dialog_w - 20;
    int input_h = 22;
    renderer_draw_rect(r, input_x, input_y, input_w, input_h, input_bg);
    renderer_draw_text(r, p->goto_input, input_x + 4, input_y + 3, input_text);

    // Draw cursor in input
    if (focused) {
      int cursor_x = input_x + 4 + (int)p->goto_cursor * char_width;
      renderer_draw_rect(r, cursor_x, input_y + 3, 2, input_h - 6, input_text);
    }
  }

  // Draw cursor (only if search/goto is not active, otherwise cursor is in
  // dialog input)
  if (!p->search_visible && !p->goto_visible && editor_cursor_is_visible(e)) {
    int cursor_x, cursor_y;
    editor_pos_to_screen(e, editor_get_cursor_pos(e), &cursor_x, &cursor_y);

    // Translate to window coordinates (add gutter and header_height for text
    // area offset)
    cursor_x += p->bounds.x + gutter_width;
    cursor_y += p->bounds.y + header_height;

    SDL_Color cursor_color = {.r = 255, .g = 255, .b = 255, .a = 255};
    if (cursor_y >= p->bounds.y + header_height &&
        cursor_y < p->bounds.y + p->bounds.h - STATUS_BAR_HEIGHT) {
      renderer_draw_rect(r, cursor_x, cursor_y, 2, line_height, cursor_color);
    }
  }
}

Editor *pane_get_editor(Pane *p) {
  if (!p) {
    return NULL;
  }
  return p->view;
}

Buffer *pane_get_buffer(Pane *p) {
  if (!p || !p->view) {
    return NULL;
  }
  return editor_get_buffer(p->view);
}

bool pane_point_in_tag_bar(Pane *p, int x, int y) {
  if (!p) {
    return false;
  }
  return x >= p->bounds.x && x < p->bounds.x + p->bounds.w &&
         y >= p->bounds.y && y < p->bounds.y + TAG_BAR_HEIGHT;
}

TagCommand pane_tag_bar_hit_test(Pane *p, int x, int y) {
  if (!p || !pane_point_in_tag_bar(p, x, y)) {
    return TAG_CMD_NONE;
  }

  int char_width = p->char_width;

  // Commands are drawn right-to-left starting from right edge
  int cmd_x = p->bounds.x + p->bounds.w - 4;

  // Check each command from right to left (reverse order of TAG_COMMANDS)
  for (int i = TAG_COMMAND_COUNT - 1; i >= 0; i--) {
    int cmd_len = (int)strlen(TAG_COMMANDS[i]);
    int cmd_start = cmd_x - cmd_len * char_width - 8;
    int cmd_end = cmd_x;

    if (x >= cmd_start && x < cmd_end) {
      // Map index to command
      switch (i) {
      case 0: return TAG_CMD_NEW;
      case 1: return TAG_CMD_OPEN;
      case 2: return TAG_CMD_SAVE;
      case 3: return TAG_CMD_SAVEAS;
      case 4: return TAG_CMD_DEL;
      }
    }

    cmd_x = cmd_start;
  }

  return TAG_CMD_NONE;
}

// === Search Functionality ===

void pane_search_show(Pane *p) {
  if (!p) {
    return;
  }
  p->search_visible = true;
  p->search_focused = true;
  p->search_cursor = strlen(p->search_query);

  // Update viewport height
  editor_set_viewport_height(p->view, p->bounds.h - pane_header_height(p) -
                                          STATUS_BAR_HEIGHT);

  // If there's already a query, search for it
  if (p->search_query[0] != '\0') {
    pane_search_find_all(p);
  }
}

void pane_search_hide(Pane *p) {
  if (!p) {
    return;
  }
  p->search_visible = false;
  p->replace_mode = false;

  // Clear search highlights
  Buffer *buf = pane_get_buffer(p);
  if (buf) {
    annot_store_clear_layer(buf->annots, "search");
  }

  // Update viewport height
  editor_set_viewport_height(p->view, p->bounds.h - pane_header_height(p) -
                                          STATUS_BAR_HEIGHT);
}

void pane_search_toggle_replace(Pane *p) {
  if (!p) {
    return;
  }
  if (!p->search_visible) {
    pane_search_show(p);
  }
  p->replace_mode = !p->replace_mode;
  if (p->replace_mode) {
    p->search_focused = false; // Focus replace input
    p->replace_cursor = strlen(p->replace_query);
  } else {
    p->search_focused = true;
  }

  // Update viewport height
  editor_set_viewport_height(p->view, p->bounds.h - pane_header_height(p) -
                                          STATUS_BAR_HEIGHT);
}

bool pane_search_is_active(const Pane *p) {
  return p && p->search_visible;
}

void pane_search_set_query(Pane *p, const char *query) {
  if (!p || !query) {
    return;
  }
  strncpy(p->search_query, query, sizeof(p->search_query) - 1);
  p->search_query[sizeof(p->search_query) - 1] = '\0';
  p->search_cursor = strlen(p->search_query);
  p->matches_dirty = true;
}

void pane_search_find_all(Pane *p) {
  if (!p) {
    return;
  }

  Buffer *buf = pane_get_buffer(p);
  if (!buf) {
    return;
  }

  Document *doc = buf->document;
  size_t doc_len = document_length(doc);
  size_t query_len = strlen(p->search_query);

  // Clear previous matches
  p->match_count = 0;
  p->current_match = SIZE_MAX;

  // Clear search annotations
  annot_store_clear_layer(buf->annots, "search");

  if (query_len == 0 || query_len > doc_len) {
    return;
  }

  // Ensure matches array has capacity
  size_t initial_cap = 64;
  if (p->match_capacity == 0) {
    p->matches = malloc(initial_cap * sizeof(SearchMatch));
    if (!p->matches) {
      return;
    }
    p->match_capacity = initial_cap;
  }

  // Search through document
  char buf_chunk[512];
  size_t pos = 0;

  while (pos + query_len <= doc_len) {
    // Get a chunk of text
    size_t chunk_len = sizeof(buf_chunk) - 1;
    if (pos + chunk_len > doc_len) {
      chunk_len = doc_len - pos;
    }

    size_t got = document_get_text(doc, pos, buf_chunk, chunk_len);
    buf_chunk[got] = '\0';

    // Search within this chunk
    char *found = buf_chunk;
    while ((found = strstr(found, p->search_query)) != NULL) {
      size_t match_pos = pos + (size_t)(found - buf_chunk);

      // Grow array if needed
      if (p->match_count >= p->match_capacity) {
        size_t new_cap = p->match_capacity * 2;
        SearchMatch *new_matches =
            realloc(p->matches, new_cap * sizeof(SearchMatch));
        if (!new_matches) {
          break;
        }
        p->matches = new_matches;
        p->match_capacity = new_cap;
      }

      // Add match
      p->matches[p->match_count].start = match_pos;
      p->matches[p->match_count].end = match_pos + query_len;
      p->match_count++;

      // Add highlight annotation
      const char *keys[] = {"style:bg"};
      const char *values[] = {"#444400"};
      annot_store_add(buf->annots, match_pos, match_pos + query_len, "search",
                      keys, values, 1);

      found++;
    }

    // Move to next chunk, overlapping by query_len - 1 to catch matches at
    // boundaries
    if (got < chunk_len || pos + got >= doc_len) {
      break;
    }
    pos += got - query_len + 1;
  }

  // Set current match to first one after cursor
  if (p->match_count > 0) {
    size_t cursor = editor_get_cursor_pos(p->view);
    p->current_match = 0;
    for (size_t i = 0; i < p->match_count; i++) {
      if (p->matches[i].start >= cursor) {
        p->current_match = i;
        break;
      }
    }
  }

  p->matches_dirty = false;
}

void pane_search_next(Pane *p) {
  if (!p || p->match_count == 0) {
    return;
  }

  if (p->matches_dirty) {
    pane_search_find_all(p);
  }

  if (p->match_count == 0) {
    return;
  }

  // Move to next match
  if (p->current_match == SIZE_MAX) {
    p->current_match = 0;
  } else {
    p->current_match = (p->current_match + 1) % p->match_count;
  }

  // Jump to match
  SearchMatch *m = &p->matches[p->current_match];
  Editor *e = pane_get_editor(p);

  // Move cursor and scroll to match
  // We need to set cursor position and ensure it's visible
  // Using a simple approach: set cursor to match start
  editor_clear_selection(e);

  // Directly set cursor position by simulating navigation
  // This is a bit hacky - ideally editor would have a set_cursor_pos function
  size_t current = editor_get_cursor_pos(e);
  if (m->start > current) {
    while (editor_get_cursor_pos(e) < m->start) {
      editor_move_cursor_right(e);
    }
  } else if (m->start < current) {
    while (editor_get_cursor_pos(e) > m->start) {
      editor_move_cursor_left(e);
    }
  }

  editor_scroll_to_cursor(e);
}

void pane_search_prev(Pane *p) {
  if (!p || p->match_count == 0) {
    return;
  }

  if (p->matches_dirty) {
    pane_search_find_all(p);
  }

  if (p->match_count == 0) {
    return;
  }

  // Move to previous match
  if (p->current_match == SIZE_MAX || p->current_match == 0) {
    p->current_match = p->match_count - 1;
  } else {
    p->current_match--;
  }

  // Jump to match (same as next)
  SearchMatch *m = &p->matches[p->current_match];
  Editor *e = pane_get_editor(p);

  editor_clear_selection(e);
  size_t current = editor_get_cursor_pos(e);
  if (m->start > current) {
    while (editor_get_cursor_pos(e) < m->start) {
      editor_move_cursor_right(e);
    }
  } else if (m->start < current) {
    while (editor_get_cursor_pos(e) > m->start) {
      editor_move_cursor_left(e);
    }
  }

  editor_scroll_to_cursor(e);
}

void pane_search_replace_one(Pane *p) {
  if (!p || p->match_count == 0 || p->current_match == SIZE_MAX) {
    return;
  }

  Editor *e = pane_get_editor(p);
  SearchMatch *m = &p->matches[p->current_match];

  // Select the match
  size_t current = editor_get_cursor_pos(e);

  // Move to match start
  editor_clear_selection(e);
  while (editor_get_cursor_pos(e) > m->start) {
    editor_move_cursor_left(e);
  }
  while (editor_get_cursor_pos(e) < m->start) {
    editor_move_cursor_right(e);
  }

  // Delete match text
  for (size_t i = 0; i < m->end - m->start; i++) {
    editor_delete_forward(e);
  }

  // Insert replacement
  size_t replace_len = strlen(p->replace_query);
  if (replace_len > 0) {
    editor_insert_text(e, p->replace_query, replace_len);
  }

  // Re-search (matches have shifted)
  pane_search_find_all(p);

  // Move to next match
  if (p->match_count > 0) {
    pane_search_next(p);
  }
}

void pane_search_replace_all(Pane *p) {
  if (!p || p->match_count == 0) {
    return;
  }

  Editor *e = pane_get_editor(p);
  size_t replace_len = strlen(p->replace_query);
  size_t query_len = strlen(p->search_query);

  // Replace from end to start to preserve positions
  for (size_t i = p->match_count; i > 0; i--) {
    SearchMatch *m = &p->matches[i - 1];

    // Move to match start
    editor_clear_selection(e);
    size_t current = editor_get_cursor_pos(e);
    while (editor_get_cursor_pos(e) > m->start) {
      editor_move_cursor_left(e);
    }
    while (editor_get_cursor_pos(e) < m->start) {
      editor_move_cursor_right(e);
    }

    // Delete match text
    for (size_t j = 0; j < query_len; j++) {
      editor_delete_forward(e);
    }

    // Insert replacement
    if (replace_len > 0) {
      editor_insert_text(e, p->replace_query, replace_len);
    }
  }

  // Clear matches
  p->match_count = 0;
  p->current_match = SIZE_MAX;

  // Clear search annotations
  Buffer *buf = pane_get_buffer(p);
  if (buf) {
    annot_store_clear_layer(buf->annots, "search");
  }
}

bool pane_search_handle_key(Pane *p, SDL_Keycode key, SDL_Keymod mod) {
  if (!p || !p->search_visible) {
    return false;
  }

  char *query = p->search_focused ? p->search_query : p->replace_query;
  size_t *cursor = p->search_focused ? &p->search_cursor : &p->replace_cursor;
  size_t max_len = sizeof(p->search_query) - 1;
  size_t len = strlen(query);

  switch (key) {
  case SDLK_ESCAPE: pane_search_hide(p); return true;

  case SDLK_RETURN:
  case SDLK_KP_ENTER:
    if (mod & SDL_KMOD_SHIFT) {
      pane_search_prev(p);
    } else {
      pane_search_next(p);
    }
    return true;

  case SDLK_TAB:
    if (p->replace_mode) {
      p->search_focused = !p->search_focused;
      if (p->search_focused) {
        p->search_cursor = strlen(p->search_query);
      } else {
        p->replace_cursor = strlen(p->replace_query);
      }
    }
    return true;

  case SDLK_BACKSPACE:
    if (*cursor > 0) {
      memmove(query + *cursor - 1, query + *cursor, len - *cursor + 1);
      (*cursor)--;
      if (p->search_focused) {
        p->matches_dirty = true;
        pane_search_find_all(p);
      }
    }
    return true;

  case SDLK_DELETE:
    if (*cursor < len) {
      memmove(query + *cursor, query + *cursor + 1, len - *cursor);
      if (p->search_focused) {
        p->matches_dirty = true;
        pane_search_find_all(p);
      }
    }
    return true;

  case SDLK_LEFT:
    if (*cursor > 0) {
      (*cursor)--;
    }
    return true;

  case SDLK_RIGHT:
    if (*cursor < len) {
      (*cursor)++;
    }
    return true;

  case SDLK_HOME: *cursor = 0; return true;

  case SDLK_END: *cursor = len; return true;

  default: return false;
  }
}

bool pane_search_handle_text(Pane *p, const char *text) {
  if (!p || !p->search_visible || !text) {
    return false;
  }

  char *query = p->search_focused ? p->search_query : p->replace_query;
  size_t *cursor = p->search_focused ? &p->search_cursor : &p->replace_cursor;
  size_t max_len = sizeof(p->search_query) - 1;
  size_t len = strlen(query);
  size_t text_len = strlen(text);

  // Don't insert if it would overflow
  if (len + text_len >= max_len) {
    return true;
  }

  // Insert text at cursor
  memmove(query + *cursor + text_len, query + *cursor, len - *cursor + 1);
  memcpy(query + *cursor, text, text_len);
  *cursor += text_len;

  // Re-search if search query changed
  if (p->search_focused) {
    p->matches_dirty = true;
    pane_search_find_all(p);
  }

  return true;
}

// === Go To Line Functionality ===

void pane_goto_show(Pane *p) {
  if (!p) {
    return;
  }

  // Hide search bar if visible
  if (p->search_visible) {
    pane_search_hide(p);
  }

  p->goto_visible = true;
  p->goto_input[0] = '\0';
  p->goto_cursor = 0;
}

void pane_goto_hide(Pane *p) {
  if (!p) {
    return;
  }
  p->goto_visible = false;
}

bool pane_goto_is_active(const Pane *p) {
  return p && p->goto_visible;
}

void pane_goto_execute(Pane *p) {
  if (!p || !p->goto_visible) {
    return;
  }

  // Parse line number
  long line_num = strtol(p->goto_input, NULL, 10);
  if (line_num <= 0) {
    pane_goto_hide(p);
    return;
  }

  Buffer *buf = pane_get_buffer(p);
  if (!buf) {
    pane_goto_hide(p);
    return;
  }

  Document *doc = buf->document;
  size_t line_count = document_line_count(doc);

  // Clamp to valid range
  size_t target_line = (size_t)line_num - 1; // Convert to 0-indexed
  if (target_line >= line_count) {
    target_line = line_count > 0 ? line_count - 1 : 0;
  }

  // Get position at start of target line
  size_t target_pos = document_line_start(doc, target_line);

  // Move cursor there
  Editor *e = pane_get_editor(p);
  editor_clear_selection(e);
  editor_set_cursor_pos(e, target_pos);
  pane_goto_hide(p);
}

bool pane_goto_handle_key(Pane *p, SDL_Keycode key, SDL_Keymod mod) {
  if (!p || !p->goto_visible) {
    return false;
  }

  size_t len = strlen(p->goto_input);

  switch (key) {
  case SDLK_ESCAPE: pane_goto_hide(p); return true;

  case SDLK_RETURN:
  case SDLK_KP_ENTER: pane_goto_execute(p); return true;

  case SDLK_BACKSPACE:
    if (p->goto_cursor > 0) {
      memmove(p->goto_input + p->goto_cursor - 1,
              p->goto_input + p->goto_cursor, len - p->goto_cursor + 1);
      p->goto_cursor--;
    }
    return true;

  case SDLK_DELETE:
    if (p->goto_cursor < len) {
      memmove(p->goto_input + p->goto_cursor,
              p->goto_input + p->goto_cursor + 1, len - p->goto_cursor);
    }
    return true;

  case SDLK_LEFT:
    if (p->goto_cursor > 0) {
      p->goto_cursor--;
    }
    return true;

  case SDLK_RIGHT:
    if (p->goto_cursor < len) {
      p->goto_cursor++;
    }
    return true;

  case SDLK_HOME: p->goto_cursor = 0; return true;

  case SDLK_END: p->goto_cursor = len; return true;

  default: return false;
  }
}

bool pane_goto_handle_text(Pane *p, const char *text) {
  if (!p || !p->goto_visible || !text) {
    return false;
  }

  // Only accept digits
  for (size_t i = 0; text[i]; i++) {
    if (!isdigit((unsigned char)text[i])) {
      return true; // Consume but ignore non-digits
    }
  }

  size_t len = strlen(p->goto_input);
  size_t text_len = strlen(text);
  size_t max_len = sizeof(p->goto_input) - 1;

  // Don't insert if it would overflow
  if (len + text_len >= max_len) {
    return true;
  }

  // Insert text at cursor
  memmove(p->goto_input + p->goto_cursor + text_len,
          p->goto_input + p->goto_cursor, len - p->goto_cursor + 1);
  memcpy(p->goto_input + p->goto_cursor, text, text_len);
  p->goto_cursor += text_len;

  return true;
}
