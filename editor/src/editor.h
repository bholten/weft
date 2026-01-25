#ifndef WEFT_EDITOR_H
#define WEFT_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buffer.h"
#include "input.h"

// Edit operation for undo/redo
typedef enum { EDIT_INSERT, EDIT_DELETE } EditType;

typedef struct EditOp {
  EditType type;
  size_t pos;          // Position in document
  char *text;          // Text that was inserted/deleted (owned)
  size_t len;          // Length of text
  struct EditOp *next; // Stack linkage
} EditOp;

typedef struct Editor {
  Buffer *buffer; // Reference (not owned, managed by BufferManager)

  // Cursor state
  size_t cursor_pos;    // Byte offset in document
  size_t target_column; // Sticky column for up/down navigation

  // Selection state
  size_t
      selection_anchor; // SIZE_MAX = no selection, otherwise start of selection
  bool mouse_dragging;  // True while dragging to select

  // Column selection state (Alt+Drag)
  bool column_mode;          // true when in column/block selection
  size_t column_anchor_line; // line number where selection started
  int column_anchor_vcol;    // visual column where selection started
  size_t column_cursor_line; // current line number
  int column_cursor_vcol;    // current visual column

  // Undo/redo stacks
  EditOp *undo_stack; // Operations that can be undone
  EditOp *redo_stack; // Operations that can be redone

  // Blink state
  bool cursor_visible;
  uint64_t last_blink_time;

  // Layout (from renderer metrics)
  int char_width;
  int line_height;
  int text_offset_x;
  int text_offset_y;

  // Scroll state
  int scroll_y;        // Vertical scroll offset in pixels
  int viewport_height; // Visible area height in pixels

  // Indent settings
  int tab_size;  // Tab width / spaces per indent (default 4)
  bool use_tabs; // Insert tabs vs spaces (default: tabs)

  // Text wrapping
  bool wrap_enabled; // Whether text wrapping is enabled
  int wrap_width;    // Character width to wrap at, 0 = use pane width

  // Visual line cache (for wrapped text)
  size_t *visual_line_starts;  // Byte offset where each visual line starts
  size_t visual_line_count;    // Number of visual lines
  size_t visual_line_cap;      // Capacity of visual_line_starts array
  uint64_t wrap_cache_doc_rev; // Document revision when cache was built
  int wrap_cache_width;        // Wrap width when cache was built
  int wrap_cache_pane_chars;   // Pane width in characters when cache was built
} Editor;

// Lifecycle
Editor *editor_new(Buffer *buffer);
void editor_free(Editor *e);
void editor_set_layout(Editor *e, int char_width, int line_height, int offset_x,
                       int offset_y);
void editor_set_viewport_height(Editor *e, int height);
void editor_set_indent(Editor *e, int tab_size, bool use_tabs);
void editor_set_wrap(Editor *e, bool enabled, int wrap_width);
bool editor_get_wrap_enabled(const Editor *e);
int editor_get_wrap_width(const Editor *e);

// Visual line cache (for wrapped text)
void editor_invalidate_wrap_cache(Editor *e);
void editor_ensure_wrap_cache(Editor *e, int pane_chars);
size_t editor_get_visual_line_count(Editor *e);
size_t editor_pos_to_visual_line(Editor *e, size_t pos);
size_t editor_visual_line_to_pos(Editor *e, size_t visual_line);

// Input handling
bool editor_handle_event(Editor *e, const InputEvent *event);
void editor_update(Editor *e, uint64_t current_time_ms); // Blink update

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
void editor_delete_backward(Editor *e);      // Backspace
void editor_delete_forward(Editor *e);       // Delete key
void editor_delete_word_backward(Editor *e); // Ctrl+Backspace
void editor_delete_word_forward(Editor *e);  // Ctrl+Delete

// Position conversion
void editor_pos_to_screen(const Editor *e, size_t pos, int *x, int *y);
size_t editor_screen_to_pos(const Editor *e, int x, int y);

// Query
size_t editor_get_cursor_pos(const Editor *e);
void editor_set_cursor_pos(Editor *e, size_t pos); // Set cursor to position
bool editor_cursor_is_visible(const Editor *e);

// Selection
bool editor_has_selection(const Editor *e);
void editor_get_selection(const Editor *e, size_t *start, size_t *end);
void editor_clear_selection(Editor *e);
void editor_select_all(Editor *e);
void editor_delete_selection(Editor *e);

// Column selection (Alt+Drag block selection)
bool editor_in_column_mode(const Editor *e);
void editor_clear_column_mode(Editor *e);
void editor_get_column_selection(const Editor *e, size_t *start_line,
                                 size_t *end_line, int *left_vcol,
                                 int *right_vcol);
int editor_pos_to_vcol(const Editor *e, size_t pos);
size_t editor_vcol_to_pos(const Editor *e, size_t line, int vcol);
void editor_column_insert_char(Editor *e, const char *text, size_t len);
void editor_column_delete_backward(Editor *e);
void editor_column_delete_forward(Editor *e);
char *editor_get_column_selection_text(
    const Editor *e); // Returns malloc'd string, caller frees

// Clipboard
bool editor_copy(
    Editor *e); // Copy selection to clipboard, returns true if copied
bool editor_cut(Editor *e);   // Cut selection to clipboard, returns true if cut
bool editor_paste(Editor *e); // Paste from clipboard, returns true if pasted

// Undo/Redo
bool editor_undo(Editor *e); // Undo last edit, returns true if undone
bool editor_redo(Editor *e); // Redo last undone edit, returns true if redone
bool editor_can_undo(const Editor *e);
bool editor_can_redo(const Editor *e);

// Buffer access
Buffer *editor_get_buffer(const Editor *e); // Get associated buffer

// Scrolling
void editor_scroll(Editor *e, int delta_y); // Scroll by delta pixels
void editor_scroll_to_cursor(Editor *e);    // Ensure cursor is visible
int editor_get_scroll_y(const Editor *e);   // Get current scroll offset

#endif // WEFT_EDITOR_H
