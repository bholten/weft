#ifndef WEFT_INPUT_H
#define WEFT_INPUT_H

#include <stdbool.h>

#include <SDL3/SDL.h>

typedef enum {
    INPUT_NONE,
    INPUT_KEY_DOWN,
    INPUT_KEY_UP,
    INPUT_TEXT,
    INPUT_MOUSE_DOWN,
    INPUT_MOUSE_UP,
    INPUT_MOUSE_MOVE,
    INPUT_SCROLL,
    INPUT_RESIZE,
    INPUT_QUIT
} InputEventType;

typedef struct {
    InputEventType type;
    union {
        struct {
            SDL_Keycode key;
            SDL_Keymod mod;
        } key;
        struct {
            char text[32];
        } text;
        struct {
            int x;
            int y;
            int button;
        } mouse;
        struct {
            float dx;
            float dy;
        } scroll;
        struct {
            int width;
            int height;
        } resize;
    };
} InputEvent;

typedef struct {
    bool keys[SDL_SCANCODE_COUNT];
    int mouse_x;
    int mouse_y;
    bool mouse_buttons[5];
} InputState;

// Initialize input state
void input_state_init(InputState *state);

// Poll for events, translating SDL events to InputEvent
// Returns true if an event was available
bool input_poll(InputEvent *event);

// Update input state based on an event
void input_state_update(InputState *state, const InputEvent *event);

#endif // WEFT_INPUT_H
