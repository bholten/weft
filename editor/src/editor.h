#ifndef WEFT_EDITOR_H
#define WEFT_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "document.h"
#include "input.h"

typedef struct Editor {
    Document *document;         // Reference (not owned)

    // Cursor state
    size_t cursor_pos;          // Byte offset in document
    size_t target_column;       // Sticky column for up/down navigation

    // Blink state
    bool cursor_visible;
    uint64_t last_blink_time;

    // Layout (from renderer metrics)
    int char_width;
    int line_height;
    int text_offset_x;
    int text_offset_y;
} Editor;

// Lifecycle
Editor *editor_new(Document *doc);
void editor_free(Editor *e);
void editor_set_layout(Editor *e, int char_width, int line_height, int offset_x, int offset_y);

// Input handling
bool editor_handle_event(Editor *e, const InputEvent *event);
void editor_update(Editor *e, uint64_t current_time_ms);  // Blink update

// Cursor movement
void editor_move_cursor_left(Editor *e);
void editor_move_cursor_right(Editor *e);
void editor_move_cursor_up(Editor *e);
void editor_move_cursor_down(Editor *e);
void editor_move_cursor_to_line_start(Editor *e);
void editor_move_cursor_to_line_end(Editor *e);

// Editing
void editor_insert_text(Editor *e, const char *text, size_t len);
void editor_delete_backward(Editor *e);  // Backspace
void editor_delete_forward(Editor *e);   // Delete key

// Position conversion
void editor_pos_to_screen(const Editor *e, size_t pos, int *x, int *y);
size_t editor_screen_to_pos(const Editor *e, int x, int y);

// Query
size_t editor_get_cursor_pos(const Editor *e);
bool editor_cursor_is_visible(const Editor *e);

#endif // WEFT_EDITOR_H
