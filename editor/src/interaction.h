#ifndef WEFT_INTERACTION_H
#define WEFT_INTERACTION_H

#include <stdbool.h>
#include <stddef.h>

#include <SDL3/SDL.h>

// Forward declarations
typedef struct Editor Editor;
typedef struct LclHost LclHost;

// Interaction event types
typedef enum {
    INTERACT_LEFT_CLICK,
    INTERACT_MIDDLE_CLICK,
    INTERACT_RIGHT_CLICK,
    INTERACT_HOVER
} InteractType;

// Interaction dispatch result
typedef enum {
    INTERACT_UNHANDLED,     // Event was not handled, fall through to default
    INTERACT_HANDLED        // Event was handled by a handler
} InteractResult;

// Interaction event context
typedef struct InteractEvent {
    InteractType type;
    size_t doc_pos;           // Position in document
    int screen_x, screen_y;   // Screen coordinates (window-local)
    SDL_Keymod modifiers;     // Ctrl, Shift, Alt

    // Context (populated before dispatch)
    bool has_selection;
    size_t sel_start, sel_end;
    char *selected_text;      // Owned, NULL if no selection
    char *word_at_pos;        // Owned, word under cursor

    // Internal state
    bool handled;             // Set by handler to mark event as consumed
} InteractEvent;

// === InteractEvent lifecycle ===

// Create a new interaction event
InteractEvent *interact_event_new(InteractType type, size_t doc_pos,
                                   int screen_x, int screen_y,
                                   SDL_Keymod modifiers);

// Populate context fields (selection, word at cursor) from editor
void interact_event_populate_context(InteractEvent *ie, Editor *editor);

// Free an interaction event
void interact_event_free(InteractEvent *ie);

// === Dispatch ===

// Dispatch an interaction event to handlers
// Returns INTERACT_HANDLED if a handler consumed the event, INTERACT_UNHANDLED otherwise
InteractResult interaction_dispatch(LclHost *lcl, Editor *editor, InteractEvent *ie);

// === Helper: Convert SDL button to InteractType ===
InteractType button_to_interact_type(int button);

#endif // WEFT_INTERACTION_H
