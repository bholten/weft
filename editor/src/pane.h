#ifndef WEFT_PANE_H
#define WEFT_PANE_H

#include <stdbool.h>

#include <SDL3/SDL.h>

#include "buffer.h"
#include "editor.h"

// Forward declarations
typedef struct Renderer Renderer;

// Pane: a view into a buffer with screen bounds
typedef struct Pane {
    Editor *view;             // Per-pane editor state (owned)
    SDL_Rect bounds;          // Screen position and size
    int char_width;           // Cached from renderer
    int line_height;          // Cached from renderer
    int text_offset_x;        // Left padding for text
    int text_offset_y;        // Top padding for text
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

// Tag bar height
#define TAG_BAR_HEIGHT 20

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

#endif // WEFT_PANE_H
