#ifndef WEFT_EDITOR_H
#define WEFT_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buffer.h"
#include "input.h"

// Edit operation for undo/redo
typedef enum {
    EDIT_INSERT,
    EDIT_DELETE
} EditType;

typedef struct EditOp {
    EditType type;
    size_t pos;             // Position in document
    char *text;             // Text that was inserted/deleted (owned)
    size_t len;             // Length of text
    struct EditOp *next;    // Stack linkage
} EditOp;

typedef struct Editor {
    Buffer *buffer;             // Reference (not owned, managed by BufferManager)

    // Cursor state
    size_t cursor_pos;          // Byte offset in document
    size_t target_column;       // Sticky column for up/down navigation

    // Selection state
    size_t selection_anchor;    // SIZE_MAX = no selection, otherwise start of selection
    bool mouse_dragging;        // True while dragging to select

    // Undo/redo stacks
    EditOp *undo_stack;         // Operations that can be undone
    EditOp *redo_stack;         // Operations that can be redone

    // Blink state
    bool cursor_visible;
    uint64_t last_blink_time;

    // Layout (from renderer metrics)
    int char_width;
    int line_height;
    int text_offset_x;
    int text_offset_y;

    // Scroll state
    int scroll_y;               // Vertical scroll offset in pixels
    int viewport_height;        // Visible area height in pixels

    // Indent settings
    int tab_size;               // Tab width / spaces per indent (default 4)
    bool use_tabs;              // Insert tabs vs spaces (default: tabs)
} Editor;

// Lifecycle
Editor *editor_new(Buffer *buffer);
void editor_free(Editor *e);
void editor_set_layout(Editor *e, int char_width, int line_height, int offset_x, int offset_y);
void editor_set_viewport_height(Editor *e, int height);
void editor_set_indent(Editor *e, int tab_size, bool use_tabs);

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
void editor_move_cursor_word_left(Editor *e);
void editor_move_cursor_word_right(Editor *e);
void editor_move_cursor_page_up(Editor *e);
void editor_move_cursor_page_down(Editor *e);

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

// Selection
bool editor_has_selection(const Editor *e);
void editor_get_selection(const Editor *e, size_t *start, size_t *end);
void editor_clear_selection(Editor *e);
void editor_select_all(Editor *e);
void editor_delete_selection(Editor *e);

// Clipboard
bool editor_copy(Editor *e);   // Copy selection to clipboard, returns true if copied
bool editor_cut(Editor *e);    // Cut selection to clipboard, returns true if cut
bool editor_paste(Editor *e);  // Paste from clipboard, returns true if pasted

// Undo/Redo
bool editor_undo(Editor *e);   // Undo last edit, returns true if undone
bool editor_redo(Editor *e);   // Redo last undone edit, returns true if redone
bool editor_can_undo(const Editor *e);
bool editor_can_redo(const Editor *e);

// Buffer access
Buffer *editor_get_buffer(const Editor *e);           // Get associated buffer

// Scrolling
void editor_scroll(Editor *e, int delta_y);           // Scroll by delta pixels
void editor_scroll_to_cursor(Editor *e);              // Ensure cursor is visible
int editor_get_scroll_y(const Editor *e);             // Get current scroll offset

#endif // WEFT_EDITOR_H
