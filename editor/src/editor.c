#include "editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "utf8.h"

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

  // Default wrap settings (disabled)
  e->wrap_enabled = false;
  e->wrap_width = 0;
  e->visual_line_starts = NULL;
  e->visual_line_count = 0;
  e->visual_line_cap = 0;
  e->wrap_cache_doc_rev = 0;
  e->wrap_cache_width = 0;
  e->wrap_cache_pane_chars = 0;

  return e;
}

void editor_free(Editor *e) {
  if (e) {
    editop_stack_free(e->undo_stack);
    editop_stack_free(e->redo_stack);
    free(e->visual_line_starts);
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

void editor_set_wrap(Editor *e, bool enabled, int wrap_width) {
  if (!e) {
    return;
  }
  bool changed = (e->wrap_enabled != enabled) || (e->wrap_width != wrap_width);
  e->wrap_enabled = enabled;
  e->wrap_width = wrap_width >= 0 ? wrap_width : 0;
  if (changed) {
    editor_invalidate_wrap_cache(e);
  }
}

bool editor_get_wrap_enabled(const Editor *e) {
  if (!e) {
    return false;
  }
  return e->wrap_enabled;
}

int editor_get_wrap_width(const Editor *e) {
  if (!e) {
    return 0;
  }
  return e->wrap_width;
}

void editor_invalidate_wrap_cache(Editor *e) {
  if (!e) {
    return;
  }
  e->wrap_cache_doc_rev = 0;
  e->wrap_cache_width = 0;
  e->wrap_cache_pane_chars = 0;
}

// Helper: Calculate effective wrap width in characters
static int get_effective_wrap_width(const Editor *e) {
  if (!e || !e->wrap_enabled) {
    return 0;
  }
  if (e->wrap_width > 0) {
    return e->wrap_width;
  }
  // Use pane width in characters (viewport_height is stored, need char_width)
  // For wrap_width == 0, we need pane width passed from caller
  // This will be handled by pane when calling rebuild
  return 80; // Default fallback
}

// Helper: Rebuild visual line cache
static void rebuild_visual_line_cache(Editor *e, int pane_chars) {
  if (!e || !e->buffer || !e->buffer->document) {
    return;
  }

  Document *doc = e->buffer->document;
  size_t doc_len = document_length(doc);
  size_t line_count = document_line_count(doc);

  // Determine wrap width
  int wrap_width = e->wrap_width > 0 ? e->wrap_width : pane_chars;
  if (wrap_width <= 0) {
    wrap_width = 80; // Fallback
  }

  // Estimate initial capacity (2x document lines should be plenty)
  size_t initial_cap = line_count * 2;
  if (initial_cap < 64) {
    initial_cap = 64;
  }

  // Resize array if needed
  if (e->visual_line_cap < initial_cap) {
    size_t *new_starts =
        realloc(e->visual_line_starts, initial_cap * sizeof(size_t));
    if (!new_starts) {
      return;
    }
    e->visual_line_starts = new_starts;
    e->visual_line_cap = initial_cap;
  }

  e->visual_line_count = 0;

  // Iterate through document lines and calculate visual lines
  for (size_t doc_line = 0; doc_line < line_count; doc_line++) {
    size_t line_start = document_line_start(doc, doc_line);
    size_t line_end = document_line_end(doc, doc_line);
    size_t line_len = line_end - line_start;

    // Get line text for visual width calculation
    char line_buf[4096];
    size_t buf_len = line_len;
    if (buf_len > sizeof(line_buf) - 1) {
      buf_len = sizeof(line_buf) - 1;
    }
    document_get_text(doc, line_start, line_buf, buf_len);

    // Calculate visual width accounting for tabs
    size_t pos = 0;
    size_t visual_col = 0;
    size_t visual_line_start_offset = 0;

    while (pos <= buf_len) {
      // Record start of this visual line
      if (visual_col == 0 || pos == 0) {
        // Ensure capacity
        if (e->visual_line_count >= e->visual_line_cap) {
          size_t new_cap = e->visual_line_cap * 2;
          size_t *new_starts =
              realloc(e->visual_line_starts, new_cap * sizeof(size_t));
          if (!new_starts) {
            return;
          }
          e->visual_line_starts = new_starts;
          e->visual_line_cap = new_cap;
        }
        e->visual_line_starts[e->visual_line_count++] =
            line_start + visual_line_start_offset;
      }

      if (pos >= buf_len) {
        break;
      }

      char c = line_buf[pos];
      size_t char_width = 1;

      if (c == '\t') {
        // Tab advances to next tab stop
        int tab_size = e->tab_size > 0 ? e->tab_size : 4;
        char_width = (size_t)(tab_size - (int)(visual_col % (size_t)tab_size));
      }

      // Check if this character would exceed wrap width
      if (visual_col + char_width > (size_t)wrap_width && visual_col > 0) {
        // Wrap here - start a new visual line
        visual_line_start_offset = pos;
        visual_col = 0;
        continue; // Re-process this character on new line
      }

      visual_col += char_width;
      pos++;
    }

    // Handle empty lines
    if (line_len == 0 && e->visual_line_count > 0 &&
        e->visual_line_starts[e->visual_line_count - 1] != line_start) {
      if (e->visual_line_count >= e->visual_line_cap) {
        size_t new_cap = e->visual_line_cap * 2;
        size_t *new_starts =
            realloc(e->visual_line_starts, new_cap * sizeof(size_t));
        if (!new_starts) {
          return;
        }
        e->visual_line_starts = new_starts;
        e->visual_line_cap = new_cap;
      }
      e->visual_line_starts[e->visual_line_count++] = line_start;
    }
  }

  // Update cache validity tracking
  e->wrap_cache_doc_rev = doc->revision;
  e->wrap_cache_width = wrap_width;
  e->wrap_cache_pane_chars = pane_chars;
}

// Helper: Ensure wrap cache is up to date
static void ensure_wrap_cache_internal(Editor *e, int pane_chars) {
  if (!e || !e->wrap_enabled) {
    return;
  }

  Document *doc = e->buffer ? e->buffer->document : NULL;
  if (!doc) {
    return;
  }

  int wrap_width = e->wrap_width > 0 ? e->wrap_width : pane_chars;

  // Check if cache is still valid
  if (e->wrap_cache_doc_rev == doc->revision &&
      e->wrap_cache_width == wrap_width &&
      e->wrap_cache_pane_chars == pane_chars && e->visual_line_count > 0) {
    return; // Cache is valid
  }

  rebuild_visual_line_cache(e, pane_chars);
}

// Public version for external callers (e.g., pane_render)
void editor_ensure_wrap_cache(Editor *e, int pane_chars) {
  ensure_wrap_cache_internal(e, pane_chars);
}

size_t editor_get_visual_line_count(Editor *e) {
  if (!e) {
    return 0;
  }
  if (!e->wrap_enabled) {
    return document_line_count(e->buffer->document);
  }
  ensure_wrap_cache_internal(e, e->wrap_cache_pane_chars);
  return e->visual_line_count;
}

size_t editor_pos_to_visual_line(Editor *e, size_t pos) {
  if (!e) {
    return 0;
  }
  if (!e->wrap_enabled) {
    return document_pos_to_line(e->buffer->document, pos);
  }

  ensure_wrap_cache_internal(e, e->wrap_cache_pane_chars);

  // Binary search for the visual line containing pos
  if (e->visual_line_count == 0) {
    return 0;
  }

  size_t low = 0;
  size_t high = e->visual_line_count;

  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (e->visual_line_starts[mid] <= pos) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  return low > 0 ? low - 1 : 0;
}

size_t editor_visual_line_to_pos(Editor *e, size_t visual_line) {
  if (!e) {
    return 0;
  }
  if (!e->wrap_enabled) {
    return document_line_start(e->buffer->document, visual_line);
  }

  ensure_wrap_cache_internal(e, e->wrap_cache_pane_chars);

  if (visual_line >= e->visual_line_count) {
    visual_line = e->visual_line_count > 0 ? e->visual_line_count - 1 : 0;
  }

  return e->visual_line_starts[visual_line];
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

  // Move by UTF-8 codepoint, not byte
  // Move back one byte first
  size_t pos = e->cursor_pos - 1;

  // Skip over any continuation bytes (10xxxxxx)
  while (pos > 0 &&
         utf8_is_continuation(document_get_byte(e->buffer->document, pos))) {
    pos--;
  }

  e->cursor_pos = pos;
  update_target_column(e);
  editor_scroll_to_cursor(e);
}

void editor_move_cursor_right(Editor *e) {
  if (!e) {
    return;
  }

  size_t doc_len = document_length(e->buffer->document);
  if (e->cursor_pos >= doc_len) {
    return;
  }

  // Move by UTF-8 codepoint, not byte
  unsigned char byte = document_get_byte(e->buffer->document, e->cursor_pos);
  int cp_len = utf8_codepoint_len(byte);
  size_t next = e->cursor_pos + cp_len;

  // Don't overshoot the end
  if (next > doc_len) {
    next = doc_len;
  }

  e->cursor_pos = next;
  update_target_column(e);
  editor_scroll_to_cursor(e);
}

void editor_move_cursor_up(Editor *e) {
  if (!e) {
    return;
  }

  if (e->wrap_enabled && e->visual_line_count > 0) {
    // Use visual lines for wrapped text
    size_t current_vline = editor_pos_to_visual_line(e, e->cursor_pos);
    if (current_vline == 0) {
      // Already on first visual line, move to start
      e->cursor_pos = 0;
      editor_scroll_to_cursor(e);
      return;
    }

    // Move to previous visual line at target column
    size_t prev_vline = current_vline - 1;
    size_t prev_vline_start = e->visual_line_starts[prev_vline];
    size_t prev_vline_end = (prev_vline + 1 < e->visual_line_count)
                                ? e->visual_line_starts[prev_vline + 1]
                                : document_length(e->buffer->document);
    size_t prev_vline_len = prev_vline_end - prev_vline_start;

    // Clamp target column to visual line length
    size_t new_col =
        (e->target_column < prev_vline_len) ? e->target_column : prev_vline_len;
    e->cursor_pos = prev_vline_start + new_col;
    editor_scroll_to_cursor(e);
    return;
  }

  // Non-wrapped: use document lines
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

  if (e->wrap_enabled && e->visual_line_count > 0) {
    // Use visual lines for wrapped text
    size_t current_vline = editor_pos_to_visual_line(e, e->cursor_pos);

    if (current_vline >= e->visual_line_count - 1) {
      // Already on last visual line, move to end
      e->cursor_pos = document_length(e->buffer->document);
      editor_scroll_to_cursor(e);
      return;
    }

    // Move to next visual line at target column
    size_t next_vline = current_vline + 1;
    size_t next_vline_start = e->visual_line_starts[next_vline];
    size_t next_vline_end = (next_vline + 1 < e->visual_line_count)
                                ? e->visual_line_starts[next_vline + 1]
                                : document_length(e->buffer->document);
    size_t next_vline_len = next_vline_end - next_vline_start;

    // Clamp target column to visual line length
    size_t new_col =
        (e->target_column < next_vline_len) ? e->target_column : next_vline_len;
    e->cursor_pos = next_vline_start + new_col;
    editor_scroll_to_cursor(e);
    return;
  }

  // Non-wrapped: use document lines
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

  if (e->wrap_enabled && e->visual_line_count > 0) {
    // Use visual lines for wrapped text
    size_t current_vline = editor_pos_to_visual_line(e, e->cursor_pos);

    // Move up by page
    size_t target_vline = 0;
    if (current_vline > (size_t)lines_per_page) {
      target_vline = current_vline - (size_t)lines_per_page;
    }

    // Position cursor at target column on new visual line
    size_t vline_start = e->visual_line_starts[target_vline];
    size_t vline_end = (target_vline + 1 < e->visual_line_count)
                           ? e->visual_line_starts[target_vline + 1]
                           : document_length(e->buffer->document);
    size_t vline_len = vline_end - vline_start;

    size_t new_col =
        (e->target_column < vline_len) ? e->target_column : vline_len;
    e->cursor_pos = vline_start + new_col;
    editor_scroll_to_cursor(e);
    return;
  }

  // Non-wrapped: use document lines
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

  if (e->wrap_enabled && e->visual_line_count > 0) {
    // Use visual lines for wrapped text
    size_t current_vline = editor_pos_to_visual_line(e, e->cursor_pos);

    // Move down by page
    size_t target_vline = current_vline + (size_t)lines_per_page;
    if (target_vline >= e->visual_line_count) {
      target_vline = e->visual_line_count - 1;
    }

    // Position cursor at target column on new visual line
    size_t vline_start = e->visual_line_starts[target_vline];
    size_t vline_end = (target_vline + 1 < e->visual_line_count)
                           ? e->visual_line_starts[target_vline + 1]
                           : document_length(e->buffer->document);
    size_t vline_len = vline_end - vline_start;

    size_t new_col =
        (e->target_column < vline_len) ? e->target_column : vline_len;
    e->cursor_pos = vline_start + new_col;
    editor_scroll_to_cursor(e);
    return;
  }

  // Non-wrapped: use document lines
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

  // Find start of UTF-8 codepoint to delete
  size_t delete_start = e->cursor_pos - 1;

  // Skip over any continuation bytes (10xxxxxx)
  while (delete_start > 0 && utf8_is_continuation(document_get_byte(
                                 e->buffer->document, delete_start))) {
    delete_start--;
  }

  size_t delete_len = e->cursor_pos - delete_start;

  // Get the bytes to be deleted for undo
  char deleted_bytes[4];
  document_get_text(e->buffer->document, delete_start, deleted_bytes,
                    delete_len);

  if (document_delete(e->buffer->document, delete_start, delete_len)) {
    annot_store_on_delete(e->buffer->annots, delete_start, delete_len);
    record_edit(e, EDIT_DELETE, delete_start, deleted_bytes, delete_len);
    e->cursor_pos = delete_start;
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

  // Get length of UTF-8 codepoint at cursor
  unsigned char byte = document_get_byte(e->buffer->document, e->cursor_pos);
  int cp_len = utf8_codepoint_len(byte);

  // Don't delete past end of document
  if (e->cursor_pos + cp_len > doc_len) {
    cp_len = doc_len - e->cursor_pos;
  }

  // Get the bytes to be deleted for undo
  char deleted_bytes[4];
  document_get_text(e->buffer->document, e->cursor_pos, deleted_bytes, cp_len);

  if (document_delete(e->buffer->document, e->cursor_pos, cp_len)) {
    annot_store_on_delete(e->buffer->annots, e->cursor_pos, cp_len);
    record_edit(e, EDIT_DELETE, e->cursor_pos, deleted_bytes, cp_len);
    update_target_column(e);
  }
}

void editor_pos_to_screen(const Editor *e, size_t pos, int *x, int *y) {
  if (!e || !x || !y) {
    return;
  }

  if (e->wrap_enabled && e->visual_line_count > 0) {
    // Use visual lines for wrapped text
    size_t visual_line = editor_pos_to_visual_line((Editor *)e, pos);
    size_t visual_line_start = e->visual_line_starts[visual_line];
    size_t offset_in_vline = pos - visual_line_start;

    // Get text from visual line start to pos for tab calculation
    char line_buf[256];
    size_t buf_len = offset_in_vline;
    if (buf_len > sizeof(line_buf) - 1) {
      buf_len = sizeof(line_buf) - 1;
    }
    document_get_text(e->buffer->document, visual_line_start, line_buf,
                      buf_len);

    // Convert to visual column (accounting for tabs)
    size_t visual_col =
        doc_col_to_visual(line_buf, buf_len, buf_len, e->tab_size);

    *x = e->text_offset_x + (int)visual_col * e->char_width;
    *y = e->text_offset_y + (int)visual_line * e->line_height - e->scroll_y;
    return;
  }

  // Non-wrapped: use document lines
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
  size_t line_idx = 0;
  if (relative_y > 0) {
    line_idx = (size_t)(relative_y / e->line_height);
  }

  if (e->wrap_enabled && e->visual_line_count > 0) {
    // Use visual lines for wrapped text
    if (line_idx >= e->visual_line_count) {
      line_idx = e->visual_line_count - 1;
    }

    size_t visual_line_start = e->visual_line_starts[line_idx];

    // Calculate the end of this visual line
    size_t visual_line_end;
    if (line_idx + 1 < e->visual_line_count) {
      visual_line_end = e->visual_line_starts[line_idx + 1];
      // Adjust for newline: if next visual line is on same doc line, end is at
      // next start Otherwise, end is at the newline (or doc end)
    } else {
      visual_line_end = document_length(e->buffer->document);
    }

    // Visual line length
    size_t vline_len = visual_line_end - visual_line_start;

    // Calculate visual column from x coordinate
    int relative_x = x - e->text_offset_x;
    size_t visual_col = 0;
    if (relative_x > 0) {
      visual_col = (size_t)(relative_x / e->char_width);
    }

    // Get visual line content for tab calculation
    char line_buf[256];
    size_t buf_len = vline_len;
    if (buf_len > sizeof(line_buf) - 1) {
      buf_len = sizeof(line_buf) - 1;
    }
    document_get_text(e->buffer->document, visual_line_start, line_buf,
                      buf_len);

    // Convert visual column to document offset (accounting for tabs)
    size_t doc_offset =
        visual_col_to_doc(line_buf, buf_len, visual_col, e->tab_size);
    if (doc_offset > vline_len) {
      doc_offset = vline_len;
    }

    return visual_line_start + doc_offset;
  }

  // Non-wrapped: use document lines
  size_t line_count = document_line_count(e->buffer->document);
  if (line_idx >= line_count) {
    line_idx = line_count - 1;
  }

  // Calculate visual column from x coordinate
  int relative_x = x - e->text_offset_x;
  size_t visual_col = 0;
  if (relative_x > 0) {
    visual_col = (size_t)(relative_x / e->char_width);
  }

  // Get line bounds and content
  size_t line_start = document_line_start(e->buffer->document, line_idx);
  size_t line_end = document_line_end(e->buffer->document, line_idx);
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
  size_t line_count;
  if (e->wrap_enabled && e->visual_line_count > 0) {
    line_count = e->visual_line_count;
  } else {
    line_count = document_line_count(e->buffer->document);
  }
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
  size_t line;
  if (e->wrap_enabled && e->visual_line_count > 0) {
    line = editor_pos_to_visual_line(e, e->cursor_pos);
  } else {
    line = document_pos_to_line(e->buffer->document, e->cursor_pos);
  }
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
