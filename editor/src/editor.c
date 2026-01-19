#include "editor.h"

#include <stdio.h>
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
  size_t line = document_pos_to_line(e->buffer->document, e->cursor_pos);
  size_t line_start = document_line_start(e->buffer->document, line);
  return e->cursor_pos - line_start;
}

// Helper: Update target column to current position
static void update_target_column(Editor *e) {
  e->target_column = get_current_column(e);
}

// Helper: Convert document byte offset within line to visual column (accounting
// for tabs)
static size_t doc_col_to_visual(const char *line_buf, size_t line_len,
                                size_t doc_col, int tab_size) {
  if (tab_size <= 0) {
    tab_size = 4;
  }

  size_t visual_col = 0;
  for (size_t i = 0; i < line_len && i < doc_col; i++) {
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

// Helper: Convert visual column to document byte offset within line (accounting
// for tabs)
static size_t visual_col_to_doc(const char *line_buf, size_t line_len,
                                size_t visual_col, int tab_size) {
  if (tab_size <= 0) {
    tab_size = 4;
  }

  size_t current_visual = 0;
  size_t doc_col = 0;

  for (doc_col = 0; doc_col < line_len; doc_col++) {
    if (current_visual >= visual_col) {
      break;
    }
    if (line_buf[doc_col] == '\t') {
      int spaces = tab_size - (int)(current_visual % (size_t)tab_size);
      current_visual += (size_t)spaces;
    } else {
      current_visual++;
    }
  }
  return doc_col;
}

// Helper: Create a new edit operation
static EditOp *editop_new(EditType type, size_t pos, const char *text,
                          size_t len) {
  EditOp *op = malloc(sizeof(EditOp));
  if (!op) {
    return NULL;
  }

  op->type = type;
  op->pos = pos;
  op->len = len;
  op->next = NULL;

  op->text = malloc(len + 1);
  if (!op->text) {
    free(op);
    return NULL;
  }
  memcpy(op->text, text, len);
  op->text[len] = '\0';

  return op;
}

// Helper: Free a single edit operation
static void editop_free(EditOp *op) {
  if (op) {
    free(op->text);
    free(op);
  }
}

// Helper: Free an entire stack of edit operations
static void editop_stack_free(EditOp *stack) {
  while (stack) {
    EditOp *next = stack->next;
    editop_free(stack);
    stack = next;
  }
}

// Helper: Push an operation onto a stack
static void editop_stack_push(EditOp **stack, EditOp *op) {
  op->next = *stack;
  *stack = op;
}

// Helper: Pop an operation from a stack (caller owns the returned op)
static EditOp *editop_stack_pop(EditOp **stack) {
  if (!*stack) {
    return NULL;
  }
  EditOp *op = *stack;
  *stack = op->next;
  op->next = NULL;
  return op;
}

// Helper: Record an edit operation for undo (clears redo stack)
static void record_edit(Editor *e, EditType type, size_t pos, const char *text,
                        size_t len) {
  EditOp *op = editop_new(type, pos, text, len);
  if (op) {
    editop_stack_push(&e->undo_stack, op);
    // Clear redo stack when new edit is made
    editop_stack_free(e->redo_stack);
    e->redo_stack = NULL;
  }
}

Editor *editor_new(Buffer *buffer) {
  if (!buffer) {
    return NULL;
  }

  Editor *e = calloc(1, sizeof(Editor));
  if (!e) {
    return NULL;
  }

  e->buffer = buffer;
  e->cursor_pos = 0;
  e->target_column = 0;
  e->selection_anchor = SIZE_MAX;
  e->mouse_dragging = false;
  e->cursor_visible = true;
  e->last_blink_time = 0;

  // Default layout values (should be set via editor_set_layout)
  e->char_width = 8;
  e->line_height = 16;
  e->text_offset_x = 10;
  e->text_offset_y = 10;

  // Default indent settings
  e->tab_size = 4;
  e->use_tabs = true;

  return e;
}

void editor_free(Editor *e) {
  if (e) {
    editop_stack_free(e->undo_stack);
    editop_stack_free(e->redo_stack);
    free(e);
  }
}

void editor_set_layout(Editor *e, int char_width, int line_height, int offset_x,
                       int offset_y) {
  if (!e) {
    return;
  }

  e->char_width = char_width;
  e->line_height = line_height;
  e->text_offset_x = offset_x;
  e->text_offset_y = offset_y;
}

void editor_set_viewport_height(Editor *e, int height) {
  if (!e) {
    return;
  }
  e->viewport_height = height;
}

void editor_set_indent(Editor *e, int tab_size, bool use_tabs) {
  if (!e) {
    return;
  }
  e->tab_size = tab_size > 0 ? tab_size : 4;
  e->use_tabs = use_tabs;
}

bool editor_handle_event(Editor *e, const InputEvent *event) {
  if (!e || !event) {
    return false;
  }

  uint64_t now = SDL_GetTicks();

  switch (event->type) {
  case INPUT_KEY_DOWN: {
    SDL_Keycode key = event->key.key;
    bool shift_held = (event->key.mod & SDL_KMOD_SHIFT) != 0;

    bool ctrl_held = (event->key.mod & SDL_KMOD_CTRL) != 0;

    switch (key) {
    case SDLK_LEFT:
      if (shift_held) {
        if (e->selection_anchor == SIZE_MAX) {
          e->selection_anchor = e->cursor_pos;
        }
      } else {
        editor_clear_selection(e);
      }
      if (ctrl_held) {
        editor_move_cursor_word_left(e);
      } else {
        editor_move_cursor_left(e);
      }
      reset_blink(e, now);
      return true;

    case SDLK_RIGHT:
      if (shift_held) {
        if (e->selection_anchor == SIZE_MAX) {
          e->selection_anchor = e->cursor_pos;
        }
      } else {
        editor_clear_selection(e);
      }
      if (ctrl_held) {
        editor_move_cursor_word_right(e);
      } else {
        editor_move_cursor_right(e);
      }
      reset_blink(e, now);
      return true;

    case SDLK_UP:
      if (shift_held) {
        if (e->selection_anchor == SIZE_MAX) {
          e->selection_anchor = e->cursor_pos;
        }
      } else {
        editor_clear_selection(e);
      }
      editor_move_cursor_up(e);
      reset_blink(e, now);
      return true;

    case SDLK_DOWN:
      if (shift_held) {
        if (e->selection_anchor == SIZE_MAX) {
          e->selection_anchor = e->cursor_pos;
        }
      } else {
        editor_clear_selection(e);
      }
      editor_move_cursor_down(e);
      reset_blink(e, now);
      return true;

    case SDLK_HOME:
      if (shift_held) {
        if (e->selection_anchor == SIZE_MAX) {
          e->selection_anchor = e->cursor_pos;
        }
      } else {
        editor_clear_selection(e);
      }
      editor_move_cursor_to_line_start(e);
      reset_blink(e, now);
      return true;

    case SDLK_END:
      if (shift_held) {
        if (e->selection_anchor == SIZE_MAX) {
          e->selection_anchor = e->cursor_pos;
        }
      } else {
        editor_clear_selection(e);
      }
      editor_move_cursor_to_line_end(e);
      reset_blink(e, now);
      return true;

    case SDLK_PAGEUP:
      if (shift_held) {
        if (e->selection_anchor == SIZE_MAX) {
          e->selection_anchor = e->cursor_pos;
        }
      } else {
        editor_clear_selection(e);
      }
      editor_move_cursor_page_up(e);
      reset_blink(e, now);
      return true;

    case SDLK_PAGEDOWN:
      if (shift_held) {
        if (e->selection_anchor == SIZE_MAX) {
          e->selection_anchor = e->cursor_pos;
        }
      } else {
        editor_clear_selection(e);
      }
      editor_move_cursor_page_down(e);
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

    case SDLK_TAB:
      if (e->use_tabs) {
        editor_insert_text(e, "\t", 1);
      } else {
        // Insert spaces instead of tab
        char spaces[16];
        int count = e->tab_size > 0 ? e->tab_size : 4;
        if (count > 16) {
          count = 16;
        }
        memset(spaces, ' ', (size_t)count);
        editor_insert_text(e, spaces, (size_t)count);
      }
      reset_blink(e, now);
      return true;

    case SDLK_A:
      if (event->key.mod & SDL_KMOD_CTRL) {
        editor_select_all(e);
        reset_blink(e, now);
        return true;
      }
      break;

    case SDLK_C:
      if (event->key.mod & SDL_KMOD_CTRL) {
        editor_copy(e);
        return true;
      }
      break;

    case SDLK_X:
      if (event->key.mod & SDL_KMOD_CTRL) {
        editor_cut(e);
        reset_blink(e, now);
        return true;
      }
      break;

    case SDLK_V:
      if (event->key.mod & SDL_KMOD_CTRL) {
        editor_paste(e);
        reset_blink(e, now);
        return true;
      }
      break;

    case SDLK_Z:
      if (event->key.mod & SDL_KMOD_CTRL) {
        if (event->key.mod & SDL_KMOD_SHIFT) {
          editor_redo(e);
        } else {
          editor_undo(e);
        }
        reset_blink(e, now);
        return true;
      }
      break;

    case SDLK_Y:
      if (event->key.mod & SDL_KMOD_CTRL) {
        editor_redo(e);
        reset_blink(e, now);
        return true;
      }
      break;

    case SDLK_S:
      if (event->key.mod & SDL_KMOD_CTRL) {
        buffer_save(e->buffer);
        return true;
      }
      break;

    default: break;
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
    if (event->mouse.button == 1) { // Left click
      size_t pos = editor_screen_to_pos(e, event->mouse.x, event->mouse.y);
      bool shift_held = (event->mouse.mod & SDL_KMOD_SHIFT) != 0;

      if (shift_held) {
        // Shift+click: extend selection from current cursor
        if (e->selection_anchor == SIZE_MAX) {
          e->selection_anchor = e->cursor_pos;
        }
      } else {
        // Plain click: start drag selection
        e->selection_anchor = pos;
        e->mouse_dragging = true;
      }

      e->cursor_pos = pos;
      update_target_column(e);
      reset_blink(e, now);
      return true;
    }
    break;
  }

  case INPUT_MOUSE_UP: {
    if (event->mouse.button == 1) {
      e->mouse_dragging = false;
      // If anchor equals cursor, clear selection (was just a click, no drag)
      if (e->selection_anchor == e->cursor_pos) {
        editor_clear_selection(e);
      }
      return true;
    }
    break;
  }

  case INPUT_MOUSE_MOVE: {
    if (e->mouse_dragging) {
      size_t pos = editor_screen_to_pos(e, event->mouse.x, event->mouse.y);
      e->cursor_pos = pos;
      update_target_column(e);
      reset_blink(e, now);
      return true;
    }
    break;
  }

  case INPUT_SCROLL: {
    // Scroll by 3 lines per scroll unit
    int scroll_amount = (int)(-event->scroll.dy * e->line_height * 3);
    editor_scroll(e, scroll_amount);
    return true;
  }

  default: break;
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
  editor_scroll_to_cursor(e);
}

void editor_move_cursor_right(Editor *e) {
  if (!e) {
    return;
  }

  size_t doc_len = document_length(e->buffer->document);
  if (e->cursor_pos < doc_len) {
    e->cursor_pos++;
    update_target_column(e);
    editor_scroll_to_cursor(e);
  }
}

void editor_move_cursor_up(Editor *e) {
  if (!e) {
    return;
  }

  size_t current_line =
      document_pos_to_line(e->buffer->document, e->cursor_pos);
  if (current_line == 0) {
    // Already on first line, move to start
    e->cursor_pos = 0;
    editor_scroll_to_cursor(e);
    return;
  }

  // Move to previous line at target column
  size_t prev_line = current_line - 1;
  size_t prev_line_start = document_line_start(e->buffer->document, prev_line);
  size_t prev_line_end = document_line_end(e->buffer->document, prev_line);
  size_t prev_line_len = prev_line_end - prev_line_start;

  // Clamp target column to line length
  size_t new_col =
      (e->target_column < prev_line_len) ? e->target_column : prev_line_len;
  e->cursor_pos = prev_line_start + new_col;
  editor_scroll_to_cursor(e);
}

void editor_move_cursor_down(Editor *e) {
  if (!e) {
    return;
  }

  size_t current_line =
      document_pos_to_line(e->buffer->document, e->cursor_pos);
  size_t line_count = document_line_count(e->buffer->document);

  if (current_line >= line_count - 1) {
    // Already on last line, move to end
    e->cursor_pos = document_length(e->buffer->document);
    editor_scroll_to_cursor(e);
    return;
  }

  // Move to next line at target column
  size_t next_line = current_line + 1;
  size_t next_line_start = document_line_start(e->buffer->document, next_line);
  size_t next_line_end = document_line_end(e->buffer->document, next_line);
  size_t next_line_len = next_line_end - next_line_start;

  // Clamp target column to line length
  size_t new_col =
      (e->target_column < next_line_len) ? e->target_column : next_line_len;
  e->cursor_pos = next_line_start + new_col;
  editor_scroll_to_cursor(e);
}

void editor_move_cursor_to_line_start(Editor *e) {
  if (!e) {
    return;
  }

  size_t current_line =
      document_pos_to_line(e->buffer->document, e->cursor_pos);
  e->cursor_pos = document_line_start(e->buffer->document, current_line);
  update_target_column(e);
  editor_scroll_to_cursor(e);
}

void editor_move_cursor_to_line_end(Editor *e) {
  if (!e) {
    return;
  }

  size_t current_line =
      document_pos_to_line(e->buffer->document, e->cursor_pos);
  e->cursor_pos = document_line_end(e->buffer->document, current_line);
  update_target_column(e);
  editor_scroll_to_cursor(e);
}

// Helper: Check if character is a word character (alphanumeric or underscore)
static bool is_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

void editor_move_cursor_word_left(Editor *e) {
  if (!e || e->cursor_pos == 0) {
    return;
  }

  size_t pos = e->cursor_pos;
  char c;

  // Skip whitespace/punctuation going backwards
  while (pos > 0) {
    document_get_text(e->buffer->document, pos - 1, &c, 1);
    if (is_word_char(c)) {
      break;
    }
    pos--;
  }

  // Skip word characters going backwards
  while (pos > 0) {
    document_get_text(e->buffer->document, pos - 1, &c, 1);
    if (!is_word_char(c)) {
      break;
    }
    pos--;
  }

  e->cursor_pos = pos;
  update_target_column(e);
  editor_scroll_to_cursor(e);
}

void editor_move_cursor_word_right(Editor *e) {
  if (!e) {
    return;
  }

  size_t doc_len = document_length(e->buffer->document);
  if (e->cursor_pos >= doc_len) {
    return;
  }

  size_t pos = e->cursor_pos;
  char c;

  // Skip current word characters
  while (pos < doc_len) {
    document_get_text(e->buffer->document, pos, &c, 1);
    if (!is_word_char(c)) {
      break;
    }
    pos++;
  }

  // Skip whitespace/punctuation
  while (pos < doc_len) {
    document_get_text(e->buffer->document, pos, &c, 1);
    if (is_word_char(c)) {
      break;
    }
    pos++;
  }

  e->cursor_pos = pos;
  update_target_column(e);
  editor_scroll_to_cursor(e);
}

void editor_move_cursor_page_up(Editor *e) {
  if (!e || e->line_height <= 0 || e->viewport_height <= 0) {
    return;
  }

  // Calculate lines per page (leave one line overlap for context)
  int lines_per_page = (e->viewport_height / e->line_height) - 1;
  if (lines_per_page < 1) {
    lines_per_page = 1;
  }

  size_t current_line =
      document_pos_to_line(e->buffer->document, e->cursor_pos);

  // Move up by page
  size_t target_line = 0;
  if (current_line > (size_t)lines_per_page) {
    target_line = current_line - (size_t)lines_per_page;
  }

  // Position cursor at target column on new line
  size_t line_start = document_line_start(e->buffer->document, target_line);
  size_t line_end = document_line_end(e->buffer->document, target_line);
  size_t line_len = line_end - line_start;

  size_t new_col = (e->target_column < line_len) ? e->target_column : line_len;
  e->cursor_pos = line_start + new_col;

  editor_scroll_to_cursor(e);
}

void editor_move_cursor_page_down(Editor *e) {
  if (!e || e->line_height <= 0 || e->viewport_height <= 0) {
    return;
  }

  // Calculate lines per page (leave one line overlap for context)
  int lines_per_page = (e->viewport_height / e->line_height) - 1;
  if (lines_per_page < 1) {
    lines_per_page = 1;
  }

  size_t current_line =
      document_pos_to_line(e->buffer->document, e->cursor_pos);
  size_t line_count = document_line_count(e->buffer->document);

  // Move down by page
  size_t target_line = current_line + (size_t)lines_per_page;
  if (target_line >= line_count) {
    target_line = line_count - 1;
  }

  // Position cursor at target column on new line
  size_t line_start = document_line_start(e->buffer->document, target_line);
  size_t line_end = document_line_end(e->buffer->document, target_line);
  size_t line_len = line_end - line_start;

  size_t new_col = (e->target_column < line_len) ? e->target_column : line_len;
  e->cursor_pos = line_start + new_col;

  editor_scroll_to_cursor(e);
}

void editor_insert_text(Editor *e, const char *text, size_t len) {
  if (!e || !text || len == 0) {
    return;
  }

  // Delete selection first if active
  if (editor_has_selection(e)) {
    editor_delete_selection(e);
  }

  size_t insert_pos = e->cursor_pos;
  if (document_insert(e->buffer->document, insert_pos, text, len)) {
    annot_store_on_insert(e->buffer->annots, insert_pos, len);
    record_edit(e, EDIT_INSERT, insert_pos, text, len);
    e->cursor_pos += len;
    update_target_column(e);
    editor_scroll_to_cursor(e);
  }
}

void editor_delete_backward(Editor *e) {
  if (!e) {
    return;
  }

  // Delete selection if active
  if (editor_has_selection(e)) {
    editor_delete_selection(e);
    return;
  }

  if (e->cursor_pos == 0) {
    return;
  }

  // Get the character to be deleted for undo
  char deleted_char;
  document_get_text(e->buffer->document, e->cursor_pos - 1, &deleted_char, 1);

  if (document_delete(e->buffer->document, e->cursor_pos - 1, 1)) {
    annot_store_on_delete(e->buffer->annots, e->cursor_pos - 1, 1);
    record_edit(e, EDIT_DELETE, e->cursor_pos - 1, &deleted_char, 1);
    e->cursor_pos--;
    update_target_column(e);
    editor_scroll_to_cursor(e);
  }
}

void editor_delete_forward(Editor *e) {
  if (!e) {
    return;
  }

  // Delete selection if active
  if (editor_has_selection(e)) {
    editor_delete_selection(e);
    return;
  }

  size_t doc_len = document_length(e->buffer->document);
  if (e->cursor_pos >= doc_len) {
    return;
  }

  // Get the character to be deleted for undo
  char deleted_char;
  document_get_text(e->buffer->document, e->cursor_pos, &deleted_char, 1);

  if (document_delete(e->buffer->document, e->cursor_pos, 1)) {
    annot_store_on_delete(e->buffer->annots, e->cursor_pos, 1);
    record_edit(e, EDIT_DELETE, e->cursor_pos, &deleted_char, 1);
    update_target_column(e);
  }
}

void editor_pos_to_screen(const Editor *e, size_t pos, int *x, int *y) {
  if (!e || !x || !y) {
    return;
  }

  size_t line = document_pos_to_line(e->buffer->document, pos);
  size_t line_start = document_line_start(e->buffer->document, line);
  size_t line_end = document_line_end(e->buffer->document, line);
  size_t doc_col = pos - line_start;
  size_t line_len = line_end - line_start;

  // Get line content for tab-aware column calculation
  char line_buf[256];
  if (line_len > sizeof(line_buf) - 1) {
    line_len = sizeof(line_buf) - 1;
  }
  document_get_text(e->buffer->document, line_start, line_buf, line_len);

  // Convert document column to visual column (accounting for tabs)
  size_t visual_col =
      doc_col_to_visual(line_buf, line_len, doc_col, e->tab_size);

  *x = e->text_offset_x + (int)visual_col * e->char_width;
  *y = e->text_offset_y + (int)line * e->line_height - e->scroll_y;
}

size_t editor_screen_to_pos(const Editor *e, int x, int y) {
  if (!e) {
    return 0;
  }

  // Calculate line from y coordinate (accounting for scroll)
  int relative_y = y + e->scroll_y - e->text_offset_y;
  size_t line = 0;
  if (relative_y > 0) {
    line = (size_t)(relative_y / e->line_height);
  }

  // Clamp to valid line range
  size_t line_count = document_line_count(e->buffer->document);
  if (line >= line_count) {
    line = line_count - 1;
  }

  // Calculate visual column from x coordinate
  int relative_x = x - e->text_offset_x;
  size_t visual_col = 0;
  if (relative_x > 0) {
    visual_col = (size_t)(relative_x / e->char_width);
  }

  // Get line bounds and content
  size_t line_start = document_line_start(e->buffer->document, line);
  size_t line_end = document_line_end(e->buffer->document, line);
  size_t line_len = line_end - line_start;

  // Get line content for tab-aware column calculation
  char line_buf[256];
  if (line_len > sizeof(line_buf) - 1) {
    line_len = sizeof(line_buf) - 1;
  }
  document_get_text(e->buffer->document, line_start, line_buf, line_len);

  // Convert visual column to document column (accounting for tabs)
  size_t doc_col =
      visual_col_to_doc(line_buf, line_len, visual_col, e->tab_size);

  if (doc_col > line_len) {
    doc_col = line_len;
  }

  return line_start + doc_col;
}

size_t editor_get_cursor_pos(const Editor *e) {
  if (!e) {
    return 0;
  }
  return e->cursor_pos;
}

void editor_set_cursor_pos(Editor *e, size_t pos) {
  if (!e) {
    return;
  }

  // Clamp to document length
  size_t doc_len = document_length(e->buffer->document);
  if (pos > doc_len) {
    pos = doc_len;
  }

  e->cursor_pos = pos;
  update_target_column(e);
  editor_scroll_to_cursor(e);
}

bool editor_cursor_is_visible(const Editor *e) {
  if (!e) {
    return false;
  }
  return e->cursor_visible;
}

bool editor_has_selection(const Editor *e) {
  if (!e) {
    return false;
  }
  return e->selection_anchor != SIZE_MAX &&
         e->selection_anchor != e->cursor_pos;
}

void editor_get_selection(const Editor *e, size_t *start, size_t *end) {
  if (!e || !start || !end) {
    return;
  }

  if (!editor_has_selection(e)) {
    *start = e->cursor_pos;
    *end = e->cursor_pos;
    return;
  }

  if (e->selection_anchor < e->cursor_pos) {
    *start = e->selection_anchor;
    *end = e->cursor_pos;
  } else {
    *start = e->cursor_pos;
    *end = e->selection_anchor;
  }
}

void editor_clear_selection(Editor *e) {
  if (!e) {
    return;
  }
  e->selection_anchor = SIZE_MAX;
}

void editor_select_all(Editor *e) {
  if (!e) {
    return;
  }
  e->selection_anchor = 0;
  e->cursor_pos = document_length(e->buffer->document);
  update_target_column(e);
}

void editor_delete_selection(Editor *e) {
  if (!e || !editor_has_selection(e)) {
    return;
  }

  size_t start, end;
  editor_get_selection(e, &start, &end);
  size_t len = end - start;

  // Get the text to be deleted for undo
  char *deleted_text = malloc(len + 1);
  if (deleted_text) {
    document_get_text(e->buffer->document, start, deleted_text, len);
    deleted_text[len] = '\0';
  }

  if (document_delete(e->buffer->document, start, len)) {
    annot_store_on_delete(e->buffer->annots, start, len);
    if (deleted_text) {
      record_edit(e, EDIT_DELETE, start, deleted_text, len);
    }
    e->cursor_pos = start;
    update_target_column(e);
    editor_scroll_to_cursor(e);
  }

  free(deleted_text);
  editor_clear_selection(e);
}

bool editor_copy(Editor *e) {
  if (!e || !editor_has_selection(e)) {
    return false;
  }

  size_t start, end;
  editor_get_selection(e, &start, &end);
  size_t len = end - start;

  // Allocate buffer for selected text (+ null terminator)
  char *buf = malloc(len + 1);
  if (!buf) {
    return false;
  }

  size_t got = document_get_text(e->buffer->document, start, buf, len);
  buf[got] = '\0';

  bool success = SDL_SetClipboardText(buf);
  free(buf);

  return success;
}

bool editor_cut(Editor *e) {
  if (!e || !editor_has_selection(e)) {
    return false;
  }

  if (editor_copy(e)) {
    editor_delete_selection(e);
    return true;
  }
  return false;
}

bool editor_paste(Editor *e) {
  if (!e) {
    return false;
  }

  if (!SDL_HasClipboardText()) {
    return false;
  }

  char *text = SDL_GetClipboardText();
  if (!text || text[0] == '\0') {
    SDL_free(text);
    return false;
  }

  editor_insert_text(e, text, strlen(text));
  SDL_free(text);
  return true;
}

bool editor_can_undo(const Editor *e) {
  return e && e->undo_stack != NULL;
}

bool editor_can_redo(const Editor *e) {
  return e && e->redo_stack != NULL;
}

bool editor_undo(Editor *e) {
  if (!e || !e->undo_stack) {
    return false;
  }

  EditOp *op = editop_stack_pop(&e->undo_stack);
  if (!op) {
    return false;
  }

  // Clear selection before undoing
  editor_clear_selection(e);

  // Apply inverse operation
  if (op->type == EDIT_INSERT) {
    // Undo insert = delete
    document_delete(e->buffer->document, op->pos, op->len);
    annot_store_on_delete(e->buffer->annots, op->pos, op->len);
    e->cursor_pos = op->pos;
  } else {
    // Undo delete = insert
    document_insert(e->buffer->document, op->pos, op->text, op->len);
    annot_store_on_insert(e->buffer->annots, op->pos, op->len);
    e->cursor_pos = op->pos + op->len;
  }
  update_target_column(e);
  editor_scroll_to_cursor(e);

  // Move to redo stack (flip the type for redo)
  op->type = (op->type == EDIT_INSERT) ? EDIT_DELETE : EDIT_INSERT;
  editop_stack_push(&e->redo_stack, op);

  return true;
}

bool editor_redo(Editor *e) {
  if (!e || !e->redo_stack) {
    return false;
  }

  EditOp *op = editop_stack_pop(&e->redo_stack);
  if (!op) {
    return false;
  }

  // Clear selection before redoing
  editor_clear_selection(e);

  // Apply operation (note: type was flipped when moved to redo stack)
  if (op->type == EDIT_INSERT) {
    // Redo insert = delete (was originally a delete that got undone)
    document_delete(e->buffer->document, op->pos, op->len);
    annot_store_on_delete(e->buffer->annots, op->pos, op->len);
    e->cursor_pos = op->pos;
  } else {
    // Redo delete = insert (was originally an insert that got undone)
    document_insert(e->buffer->document, op->pos, op->text, op->len);
    annot_store_on_insert(e->buffer->annots, op->pos, op->len);
    e->cursor_pos = op->pos + op->len;
  }
  update_target_column(e);
  editor_scroll_to_cursor(e);

  // Move back to undo stack (flip the type back)
  op->type = (op->type == EDIT_INSERT) ? EDIT_DELETE : EDIT_INSERT;
  editop_stack_push(&e->undo_stack, op);

  return true;
}

Buffer *editor_get_buffer(const Editor *e) {
  if (!e) {
    return NULL;
  }
  return e->buffer;
}

int editor_get_scroll_y(const Editor *e) {
  if (!e) {
    return 0;
  }
  return e->scroll_y;
}

void editor_scroll(Editor *e, int delta_y) {
  if (!e) {
    return;
  }

  e->scroll_y += delta_y;

  // Clamp to valid range
  if (e->scroll_y < 0) {
    e->scroll_y = 0;
  }

  // Calculate max scroll (total content height - viewport height)
  size_t line_count = document_line_count(e->buffer->document);
  int content_height = (int)line_count * e->line_height + e->text_offset_y * 2;
  int max_scroll = content_height - e->viewport_height;
  if (max_scroll < 0) {
    max_scroll = 0;
  }

  if (e->scroll_y > max_scroll) {
    e->scroll_y = max_scroll;
  }
}

void editor_scroll_to_cursor(Editor *e) {
  if (!e || e->viewport_height <= 0) {
    return;
  }

  // Get cursor screen position (without scroll adjustment for this calculation)
  size_t line = document_pos_to_line(e->buffer->document, e->cursor_pos);
  int cursor_y = e->text_offset_y + (int)line * e->line_height;

  // Check if cursor is above visible area
  if (cursor_y < e->scroll_y + e->text_offset_y) {
    e->scroll_y = cursor_y - e->text_offset_y;
  }

  // Check if cursor is below visible area
  int cursor_bottom = cursor_y + e->line_height;
  int visible_bottom = e->scroll_y + e->viewport_height - e->text_offset_y;
  if (cursor_bottom > visible_bottom) {
    e->scroll_y = cursor_bottom - e->viewport_height + e->text_offset_y;
  }

  // Clamp scroll
  if (e->scroll_y < 0) {
    e->scroll_y = 0;
  }
}
