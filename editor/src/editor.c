#include "editor.h"

#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define BLINK_INTERVAL_MS 530

// Helper: Reset cursor blink state (makes cursor visible)
static void reset_blink(Editor *e, uint64_t current_time_ms) {
    e->cursor_visible = true;
    e->last_blink_time = current_time_ms;
}

// Helper: Get current column (byte offset from line start)
static size_t get_current_column(const Editor *e) {
    size_t line = document_pos_to_line(e->document, e->cursor_pos);
    size_t line_start = document_line_start(e->document, line);
    return e->cursor_pos - line_start;
}

// Helper: Update target column to current position
static void update_target_column(Editor *e) {
    e->target_column = get_current_column(e);
}

Editor *editor_new(Document *doc) {
    if (!doc) {
        return NULL;
    }

    Editor *e = calloc(1, sizeof(Editor));
    if (!e) {
        return NULL;
    }

    e->document = doc;
    e->cursor_pos = 0;
    e->target_column = 0;
    e->cursor_visible = true;
    e->last_blink_time = 0;

    // Default layout values (should be set via editor_set_layout)
    e->char_width = 8;
    e->line_height = 16;
    e->text_offset_x = 10;
    e->text_offset_y = 10;

    return e;
}

void editor_free(Editor *e) {
    free(e);
}

void editor_set_layout(Editor *e, int char_width, int line_height, int offset_x, int offset_y) {
    if (!e) {
        return;
    }

    e->char_width = char_width;
    e->line_height = line_height;
    e->text_offset_x = offset_x;
    e->text_offset_y = offset_y;
}

bool editor_handle_event(Editor *e, const InputEvent *event) {
    if (!e || !event) {
        return false;
    }

    uint64_t now = SDL_GetTicks();

    switch (event->type) {
        case INPUT_KEY_DOWN: {
            SDL_Keycode key = event->key.key;

            switch (key) {
                case SDLK_LEFT:
                    editor_move_cursor_left(e);
                    reset_blink(e, now);
                    return true;

                case SDLK_RIGHT:
                    editor_move_cursor_right(e);
                    reset_blink(e, now);
                    return true;

                case SDLK_UP:
                    editor_move_cursor_up(e);
                    reset_blink(e, now);
                    return true;

                case SDLK_DOWN:
                    editor_move_cursor_down(e);
                    reset_blink(e, now);
                    return true;

                case SDLK_HOME:
                    editor_move_cursor_to_line_start(e);
                    reset_blink(e, now);
                    return true;

                case SDLK_END:
                    editor_move_cursor_to_line_end(e);
                    reset_blink(e, now);
                    return true;

                case SDLK_BACKSPACE:
                    editor_delete_backward(e);
                    reset_blink(e, now);
                    return true;

                case SDLK_DELETE:
                    editor_delete_forward(e);
                    reset_blink(e, now);
                    return true;

                case SDLK_RETURN:
                    editor_insert_text(e, "\n", 1);
                    reset_blink(e, now);
                    return true;

                default:
                    break;
            }
            break;
        }

        case INPUT_TEXT: {
            size_t len = strlen(event->text.text);
            if (len > 0) {
                editor_insert_text(e, event->text.text, len);
                reset_blink(e, now);
                return true;
            }
            break;
        }

        case INPUT_MOUSE_DOWN: {
            if (event->mouse.button == 1) {  // Left click
                size_t pos = editor_screen_to_pos(e, event->mouse.x, event->mouse.y);
                e->cursor_pos = pos;
                update_target_column(e);
                reset_blink(e, now);
                return true;
            }
            break;
        }

        default:
            break;
    }

    return false;
}

void editor_update(Editor *e, uint64_t current_time_ms) {
    if (!e) {
        return;
    }

    uint64_t elapsed = current_time_ms - e->last_blink_time;
    if (elapsed >= BLINK_INTERVAL_MS) {
        e->cursor_visible = !e->cursor_visible;
        e->last_blink_time = current_time_ms;
    }
}

void editor_move_cursor_left(Editor *e) {
    if (!e || e->cursor_pos == 0) {
        return;
    }

    e->cursor_pos--;
    update_target_column(e);
}

void editor_move_cursor_right(Editor *e) {
    if (!e) {
        return;
    }

    size_t doc_len = document_length(e->document);
    if (e->cursor_pos < doc_len) {
        e->cursor_pos++;
        update_target_column(e);
    }
}

void editor_move_cursor_up(Editor *e) {
    if (!e) {
        return;
    }

    size_t current_line = document_pos_to_line(e->document, e->cursor_pos);
    if (current_line == 0) {
        // Already on first line, move to start
        e->cursor_pos = 0;
        return;
    }

    // Move to previous line at target column
    size_t prev_line = current_line - 1;
    size_t prev_line_start = document_line_start(e->document, prev_line);
    size_t prev_line_end = document_line_end(e->document, prev_line);
    size_t prev_line_len = prev_line_end - prev_line_start;

    // Clamp target column to line length
    size_t new_col = (e->target_column < prev_line_len) ? e->target_column : prev_line_len;
    e->cursor_pos = prev_line_start + new_col;
}

void editor_move_cursor_down(Editor *e) {
    if (!e) {
        return;
    }

    size_t current_line = document_pos_to_line(e->document, e->cursor_pos);
    size_t line_count = document_line_count(e->document);

    if (current_line >= line_count - 1) {
        // Already on last line, move to end
        e->cursor_pos = document_length(e->document);
        return;
    }

    // Move to next line at target column
    size_t next_line = current_line + 1;
    size_t next_line_start = document_line_start(e->document, next_line);
    size_t next_line_end = document_line_end(e->document, next_line);
    size_t next_line_len = next_line_end - next_line_start;

    // Clamp target column to line length
    size_t new_col = (e->target_column < next_line_len) ? e->target_column : next_line_len;
    e->cursor_pos = next_line_start + new_col;
}

void editor_move_cursor_to_line_start(Editor *e) {
    if (!e) {
        return;
    }

    size_t current_line = document_pos_to_line(e->document, e->cursor_pos);
    e->cursor_pos = document_line_start(e->document, current_line);
    update_target_column(e);
}

void editor_move_cursor_to_line_end(Editor *e) {
    if (!e) {
        return;
    }

    size_t current_line = document_pos_to_line(e->document, e->cursor_pos);
    e->cursor_pos = document_line_end(e->document, current_line);
    update_target_column(e);
}

void editor_insert_text(Editor *e, const char *text, size_t len) {
    if (!e || !text || len == 0) {
        return;
    }

    if (document_insert(e->document, e->cursor_pos, text, len)) {
        e->cursor_pos += len;
        update_target_column(e);
    }
}

void editor_delete_backward(Editor *e) {
    if (!e || e->cursor_pos == 0) {
        return;
    }

    if (document_delete(e->document, e->cursor_pos - 1, 1)) {
        e->cursor_pos--;
        update_target_column(e);
    }
}

void editor_delete_forward(Editor *e) {
    if (!e) {
        return;
    }

    size_t doc_len = document_length(e->document);
    if (e->cursor_pos >= doc_len) {
        return;
    }

    document_delete(e->document, e->cursor_pos, 1);
    update_target_column(e);
}

void editor_pos_to_screen(const Editor *e, size_t pos, int *x, int *y) {
    if (!e || !x || !y) {
        return;
    }

    size_t line = document_pos_to_line(e->document, pos);
    size_t line_start = document_line_start(e->document, line);
    size_t col = pos - line_start;

    *x = e->text_offset_x + (int)col * e->char_width;
    *y = e->text_offset_y + (int)line * e->line_height;
}

size_t editor_screen_to_pos(const Editor *e, int x, int y) {
    if (!e) {
        return 0;
    }

    // Calculate line from y coordinate
    int relative_y = y - e->text_offset_y;
    size_t line = 0;
    if (relative_y > 0) {
        line = (size_t)(relative_y / e->line_height);
    }

    // Clamp to valid line range
    size_t line_count = document_line_count(e->document);
    if (line >= line_count) {
        line = line_count - 1;
    }

    // Calculate column from x coordinate
    int relative_x = x - e->text_offset_x;
    size_t col = 0;
    if (relative_x > 0) {
        col = (size_t)(relative_x / e->char_width);
    }

    // Get line bounds and clamp column
    size_t line_start = document_line_start(e->document, line);
    size_t line_end = document_line_end(e->document, line);
    size_t line_len = line_end - line_start;

    if (col > line_len) {
        col = line_len;
    }

    return line_start + col;
}

size_t editor_get_cursor_pos(const Editor *e) {
    if (!e) {
        return 0;
    }
    return e->cursor_pos;
}

bool editor_cursor_is_visible(const Editor *e) {
    if (!e) {
        return false;
    }
    return e->cursor_visible;
}
