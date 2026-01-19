#ifndef WEFT_PANE_H
#define WEFT_PANE_H

#include <stdbool.h>

#include <SDL3/SDL.h>

#include "buffer.h"
#include "editor.h"
#include "layer_stack.h"

// Forward declarations
typedef struct Renderer Renderer;

// Search match position
typedef struct {
    size_t start;
    size_t end;
} SearchMatch;

// Pane: a view into a buffer with screen bounds
typedef struct Pane {
    Editor *view;             // Per-pane editor state (owned)
    LayerStack *layer_stack;  // View layer stack for rendering (owned)
    SDL_Rect bounds;          // Screen position and size
    int char_width;           // Cached from renderer
    int line_height;          // Cached from renderer
    int text_offset_x;        // Left padding for text
    int text_offset_y;        // Top padding for text

    // Search state
    bool search_visible;      // Whether search bar is shown
    bool replace_mode;        // Whether replace input is shown
    char search_query[256];   // Current search query
    char replace_query[256];  // Current replace text
    size_t search_cursor;     // Cursor position in search input
    size_t replace_cursor;    // Cursor position in replace input
    bool search_focused;      // true = search input focused, false = replace input

    // Search results
    SearchMatch *matches;     // Array of match positions
    size_t match_count;       // Number of matches
    size_t match_capacity;    // Capacity of matches array
    size_t current_match;     // Index of current match (SIZE_MAX = none)
    bool matches_dirty;       // Need to re-search

    // Go to line dialog state
    bool goto_visible;        // Whether goto dialog is shown
    char goto_input[16];      // Line number input buffer
    size_t goto_cursor;       // Cursor position in goto input
} Pane;

// Lifecycle
Pane *pane_new(Buffer *buffer, int char_width, int line_height);
void pane_free(Pane *p);

// Bounds management
void pane_set_bounds(Pane *p, SDL_Rect bounds);
SDL_Rect pane_get_bounds(const Pane *p);

// Hit testing
bool pane_contains_point(const Pane *p, int x, int y);

// Coordinate translation (window coords -> pane-local coords)
void pane_window_to_local(const Pane *p, int wx, int wy, int *lx, int *ly);
void pane_local_to_window(const Pane *p, int lx, int ly, int *wx, int *wy);

// Tag bar, search bar, and status bar heights
#define TAG_BAR_HEIGHT 20
#define SEARCH_BAR_HEIGHT 24
#define STATUS_BAR_HEIGHT 18

// Minimum gutter width (for line numbers)
#define MIN_GUTTER_WIDTH 40

// Tag bar command types (for click handling)
typedef enum {
    TAG_CMD_NONE,
    TAG_CMD_NEW,
    TAG_CMD_OPEN,
    TAG_CMD_SAVE,
    TAG_CMD_SAVEAS,
    TAG_CMD_DEL
} TagCommand;

// Rendering
void pane_render(Pane *p, Renderer *r, bool focused);

// Tag bar interaction - returns command if click is in tag bar, TAG_CMD_NONE otherwise
TagCommand pane_tag_bar_hit_test(Pane *p, int x, int y);

// Check if point is in tag bar area
bool pane_point_in_tag_bar(Pane *p, int x, int y);

// Editor access
Editor *pane_get_editor(Pane *p);
Buffer *pane_get_buffer(Pane *p);

// Search functionality
void pane_search_show(Pane *p);           // Show search bar (Ctrl+F)
void pane_search_hide(Pane *p);           // Hide search bar (Escape)
void pane_search_toggle_replace(Pane *p); // Toggle replace mode (Ctrl+H)
bool pane_search_is_active(const Pane *p); // Is search bar visible?

void pane_search_set_query(Pane *p, const char *query);  // Update search query
void pane_search_find_all(Pane *p);       // Find all matches
void pane_search_next(Pane *p);           // Jump to next match
void pane_search_prev(Pane *p);           // Jump to previous match

void pane_search_replace_one(Pane *p);    // Replace current match
void pane_search_replace_all(Pane *p);    // Replace all matches

// Search input handling (returns true if event was consumed)
bool pane_search_handle_key(Pane *p, SDL_Keycode key, SDL_Keymod mod);
bool pane_search_handle_text(Pane *p, const char *text);

// Go to line functionality
void pane_goto_show(Pane *p);             // Show goto dialog (Ctrl+G)
void pane_goto_hide(Pane *p);             // Hide goto dialog
bool pane_goto_is_active(const Pane *p);  // Is goto dialog visible?
void pane_goto_execute(Pane *p);          // Jump to the line

// Goto input handling (returns true if event was consumed)
bool pane_goto_handle_key(Pane *p, SDL_Keycode key, SDL_Keymod mod);
bool pane_goto_handle_text(Pane *p, const char *text);

#endif // WEFT_PANE_H
