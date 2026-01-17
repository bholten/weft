#include "pane.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "annot_store.h"
#include "document.h"
#include "renderer.h"

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
    size_t start;       // Byte offset in document
    size_t end;         // Byte offset (exclusive)
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
    struct { const char *name; uint8_t r, g, b; } colors[] = {
        { "red",     255, 0,   0   },
        { "green",   0,   255, 0   },
        { "blue",    0,   0,   255 },
        { "yellow",  255, 255, 0   },
        { "cyan",    0,   255, 255 },
        { "magenta", 255, 0,   255 },
        { "white",   255, 255, 255 },
        { "black",   0,   0,   0   },
        { "orange",  255, 165, 0   },
        { "purple",  128, 0,   128 },
        { "pink",    255, 192, 203 },
        { "gray",    128, 128, 128 },
        { "grey",    128, 128, 128 },
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
static void style_from_annot(const AnnotStore *store, uint32_t annot_id, TextStyle *style) {
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

// Tag bar command strings and their positions (computed during render)
static const char *TAG_COMMANDS[] = { "New", "Open", "Save", "SaveAs", "Del" };
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

    p->char_width = char_width;
    p->line_height = line_height;
    p->text_offset_x = TEXT_PADDING;
    p->text_offset_y = TEXT_PADDING;

    // Set editor layout metrics
    editor_set_layout(p->view, char_width, line_height, p->text_offset_x, p->text_offset_y);

    // Default bounds (will be set properly by layout)
    p->bounds = (SDL_Rect){ .x = 0, .y = 0, .w = 800, .h = 600 };
    editor_set_viewport_height(p->view, p->bounds.h - TAG_BAR_HEIGHT);

    return p;
}

void pane_free(Pane *p) {
    if (p) {
        editor_free(p->view);
        free(p);
    }
}

void pane_set_bounds(Pane *p, SDL_Rect bounds) {
    if (!p) {
        return;
    }
    p->bounds = bounds;
    editor_set_viewport_height(p->view, bounds.h - TAG_BAR_HEIGHT);
}

SDL_Rect pane_get_bounds(const Pane *p) {
    if (!p) {
        return (SDL_Rect){ 0, 0, 0, 0 };
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
    *lx = wx - p->bounds.x;
    *ly = wy - p->bounds.y - TAG_BAR_HEIGHT;  // Account for tag bar
}

void pane_local_to_window(const Pane *p, int lx, int ly, int *wx, int *wy) {
    if (!p || !wx || !wy) {
        return;
    }
    *wx = lx + p->bounds.x;
    *wy = ly + p->bounds.y + TAG_BAR_HEIGHT;  // Account for tag bar
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
    SDL_Color bg_color = { .r = 30, .g = 30, .b = 30, .a = 255 };
    renderer_draw_rect(r, p->bounds.x, p->bounds.y, p->bounds.w, p->bounds.h, bg_color);

    // Draw tag bar background
    SDL_Color tag_bg = focused
        ? (SDL_Color){ .r = 70, .g = 130, .b = 180, .a = 255 }   // Blue when focused
        : (SDL_Color){ .r = 60, .g = 60, .b = 70, .a = 255 };    // Gray when not focused
    renderer_draw_rect(r, p->bounds.x, p->bounds.y, p->bounds.w, TAG_BAR_HEIGHT, tag_bg);

    // Draw tag bar content
    SDL_Color tag_text = { .r = 255, .g = 255, .b = 255, .a = 255 };
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
        cmd_x -= cmd_len * char_width + 8;  // 8px spacing between commands
        renderer_draw_text(r, TAG_COMMANDS[i], cmd_x, tag_y, tag_text);
    }

    // Draw focus indicator (border) - but not on top where tag bar is
    if (focused) {
        SDL_Color focus_color = { .r = 70, .g = 130, .b = 180, .a = 255 };
        // Bottom border
        renderer_draw_rect(r, p->bounds.x, p->bounds.y + p->bounds.h - 2, p->bounds.w, 2, focus_color);
        // Left border (below tag bar)
        renderer_draw_rect(r, p->bounds.x, p->bounds.y + TAG_BAR_HEIGHT, 2, p->bounds.h - TAG_BAR_HEIGHT, focus_color);
        // Right border (below tag bar)
        renderer_draw_rect(r, p->bounds.x + p->bounds.w - 2, p->bounds.y + TAG_BAR_HEIGHT, 2, p->bounds.h - TAG_BAR_HEIGHT, focus_color);
    }

    // Calculate text area (inside padding, below tag bar)
    int text_x = p->bounds.x + p->text_offset_x;
    int text_y = p->bounds.y + TAG_BAR_HEIGHT + p->text_offset_y;

    // Draw selection highlight
    if (editor_has_selection(e)) {
        size_t sel_start, sel_end;
        editor_get_selection(e, &sel_start, &sel_end);

        size_t start_line = document_pos_to_line(doc, sel_start);
        size_t end_line = document_pos_to_line(doc, sel_end);

        SDL_Color sel_color = { .r = 70, .g = 130, .b = 180, .a = 180 };

        for (size_t line = start_line; line <= end_line; line++) {
            size_t line_start_pos = document_line_start(doc, line);
            size_t line_end_pos = document_line_end(doc, line);

            // Determine highlight start/end within this line
            size_t highlight_start = (sel_start > line_start_pos) ? sel_start : line_start_pos;
            size_t highlight_end = (sel_end < line_end_pos) ? sel_end : line_end_pos;

            if (highlight_start < highlight_end) {
                int start_col = (int)(highlight_start - line_start_pos);
                int end_col = (int)(highlight_end - line_start_pos);

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
    SDL_Color default_fg = { .r = 255, .g = 255, .b = 255, .a = 255 };
    int y = text_y - scroll_y;

    size_t line_count = document_line_count(doc);
    char line_buf[256];

    // Query annotations for style layer in visible range
    AnnotQuery style_annots;
    annot_query_init(&style_annots);

    // Calculate visible document range
    size_t first_vis_line = (scroll_y > 0) ? (size_t)(scroll_y / line_height) : 0;
    if (first_vis_line > 0 && first_vis_line > 0) {
        first_vis_line--;  // Include one line before for partial visibility
    }
    size_t last_vis_line = first_vis_line + (size_t)(p->bounds.h / line_height) + 2;
    if (last_vis_line >= line_count) {
        last_vis_line = line_count > 0 ? line_count - 1 : 0;
    }

    size_t vis_start = document_line_start(doc, first_vis_line);
    size_t vis_end = document_line_end(doc, last_vis_line);

    annot_store_in_range(buf->annots, vis_start, vis_end, "style", &style_annots);

    for (size_t i = 0; i < line_count; i++) {
        // Skip lines that are off-screen (optimization)
        int line_bottom = y + line_height;
        if (line_bottom < p->bounds.y) {
            y += line_height;
            continue;
        }
        if (y > p->bounds.y + p->bounds.h) {
            break;
        }

        size_t line_start = document_line_start(doc, i);
        size_t line_end = document_line_end(doc, i);
        size_t line_len = line_end - line_start;

        if (line_len > sizeof(line_buf) - 1) {
            line_len = sizeof(line_buf) - 1;
        }

        size_t got = document_get_text(doc, line_start, line_buf, line_len);
        line_buf[got] = '\0';

        // Collect styled spans for this line
        StyledSpan spans[MAX_STYLED_SPANS];
        size_t span_count = 0;

        // Find annotations that overlap this line
        for (size_t ai = 0; ai < style_annots.count && span_count < MAX_STYLED_SPANS; ai++) {
            size_t ann_start, ann_end;
            if (!annot_store_get_span(buf->annots, style_annots.ids[ai], &ann_start, &ann_end)) {
                continue;
            }

            // Check overlap with this line
            if (ann_start >= line_end || ann_end <= line_start) {
                continue;
            }

            // Clamp to line bounds
            size_t span_start = (ann_start > line_start) ? ann_start : line_start;
            size_t span_end = (ann_end < line_end) ? ann_end : line_end;

            if (span_end - span_start > line_len) {
                span_end = line_start + line_len;
            }

            spans[span_count].start = span_start;
            spans[span_count].end = span_end;
            style_from_annot(buf->annots, style_annots.ids[ai], &spans[span_count].style);
            span_count++;
        }

        if (span_count == 0) {
            // No annotations on this line - render normally
            renderer_draw_text(r, line_buf, text_x, y, default_fg);
        } else {
            // Render line in segments
            size_t pos = line_start;
            int x = text_x;

            while (pos < line_start + got) {
                // Find next styled span starting at or after pos
                StyledSpan *active = NULL;
                for (size_t si = 0; si < span_count; si++) {
                    if (spans[si].start <= pos && spans[si].end > pos) {
                        active = &spans[si];
                        break;
                    }
                }

                size_t seg_end;
                TextStyle *style = NULL;

                if (active) {
                    // Render styled segment
                    seg_end = active->end;
                    style = &active->style;
                } else {
                    // Find start of next styled span
                    seg_end = line_start + got;
                    for (size_t si = 0; si < span_count; si++) {
                        if (spans[si].start > pos && spans[si].start < seg_end) {
                            seg_end = spans[si].start;
                        }
                    }
                }

                // Clamp to line end
                if (seg_end > line_start + got) {
                    seg_end = line_start + got;
                }

                size_t seg_len = seg_end - pos;
                size_t buf_offset = pos - line_start;

                if (seg_len > 0 && buf_offset < got) {
                    // Draw background if styled
                    if (style && style->has_bg) {
                        int seg_width = (int)seg_len * char_width;
                        renderer_draw_rect(r, x, y, seg_width, line_height, style->bg);
                    }

                    // Draw text
                    SDL_Color fg = (style && style->has_fg) ? style->fg : default_fg;
                    renderer_draw_text_n(r, line_buf + buf_offset, seg_len, x, y, fg);

                    // Draw underline if styled
                    if (style && style->underline) {
                        int seg_width = (int)seg_len * char_width;
                        int ul_y = y + line_height - 2;
                        renderer_draw_line(r, x, ul_y, x + seg_width, ul_y, fg);
                    }

                    x += (int)seg_len * char_width;
                }

                pos = seg_end;
            }
        }

        y += line_height;
    }

    annot_query_free(&style_annots);

    // Draw cursor
    if (editor_cursor_is_visible(e)) {
        int cursor_x, cursor_y;
        editor_pos_to_screen(e, editor_get_cursor_pos(e), &cursor_x, &cursor_y);

        // Translate to window coordinates (add TAG_BAR_HEIGHT for text area offset)
        cursor_x += p->bounds.x;
        cursor_y += p->bounds.y + TAG_BAR_HEIGHT;

        SDL_Color cursor_color = { .r = 255, .g = 255, .b = 255, .a = 255 };
        if (cursor_y >= p->bounds.y + TAG_BAR_HEIGHT && cursor_y < p->bounds.y + p->bounds.h) {
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
