#include "input.h"

#include <string.h>

void input_state_init(InputState *state) {
    if (!state) {
        return;
    }

    memset(state->keys, 0, sizeof(state->keys));
    state->mouse_x = 0;
    state->mouse_y = 0;
    memset(state->mouse_buttons, 0, sizeof(state->mouse_buttons));
}

bool input_poll(InputEvent *event) {
    if (!event) {
        return false;
    }

    SDL_Event sdl_event;
    if (!SDL_PollEvent(&sdl_event)) {
        return false;
    }

    event->type = INPUT_NONE;

    switch (sdl_event.type) {
        case SDL_EVENT_QUIT:
            event->type = INPUT_QUIT;
            break;

        case SDL_EVENT_KEY_DOWN:
            event->type = INPUT_KEY_DOWN;
            event->key.key = sdl_event.key.key;
            event->key.mod = sdl_event.key.mod;
            break;

        case SDL_EVENT_KEY_UP:
            event->type = INPUT_KEY_UP;
            event->key.key = sdl_event.key.key;
            event->key.mod = sdl_event.key.mod;
            break;

        case SDL_EVENT_TEXT_INPUT:
            event->type = INPUT_TEXT;
            strncpy(event->text.text, sdl_event.text.text, sizeof(event->text.text) - 1);
            event->text.text[sizeof(event->text.text) - 1] = '\0';
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            event->type = INPUT_MOUSE_DOWN;
            event->mouse.x = (int)sdl_event.button.x;
            event->mouse.y = (int)sdl_event.button.y;
            event->mouse.button = sdl_event.button.button;
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            event->type = INPUT_MOUSE_UP;
            event->mouse.x = (int)sdl_event.button.x;
            event->mouse.y = (int)sdl_event.button.y;
            event->mouse.button = sdl_event.button.button;
            break;

        case SDL_EVENT_MOUSE_MOTION:
            event->type = INPUT_MOUSE_MOVE;
            event->mouse.x = (int)sdl_event.motion.x;
            event->mouse.y = (int)sdl_event.motion.y;
            event->mouse.button = 0;
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            event->type = INPUT_SCROLL;
            event->scroll.dx = sdl_event.wheel.x;
            event->scroll.dy = sdl_event.wheel.y;
            break;

        case SDL_EVENT_WINDOW_RESIZED:
            event->type = INPUT_RESIZE;
            event->resize.width = sdl_event.window.data1;
            event->resize.height = sdl_event.window.data2;
            break;

        default:
            // Unhandled event type
            event->type = INPUT_NONE;
            break;
    }

    return true;
}

void input_state_update(InputState *state, const InputEvent *event) {
    if (!state || !event) {
        return;
    }

    switch (event->type) {
        case INPUT_KEY_DOWN: {
            SDL_Scancode scancode = SDL_GetScancodeFromKey(event->key.key, NULL);
            if (scancode < SDL_SCANCODE_COUNT) {
                state->keys[scancode] = true;
            }
            break;
        }

        case INPUT_KEY_UP: {
            SDL_Scancode scancode = SDL_GetScancodeFromKey(event->key.key, NULL);
            if (scancode < SDL_SCANCODE_COUNT) {
                state->keys[scancode] = false;
            }
            break;
        }

        case INPUT_MOUSE_DOWN:
            state->mouse_x = event->mouse.x;
            state->mouse_y = event->mouse.y;
            if (event->mouse.button >= 1 && event->mouse.button <= 5) {
                state->mouse_buttons[event->mouse.button - 1] = true;
            }
            break;

        case INPUT_MOUSE_UP:
            state->mouse_x = event->mouse.x;
            state->mouse_y = event->mouse.y;
            if (event->mouse.button >= 1 && event->mouse.button <= 5) {
                state->mouse_buttons[event->mouse.button - 1] = false;
            }
            break;

        case INPUT_MOUSE_MOVE:
            state->mouse_x = event->mouse.x;
            state->mouse_y = event->mouse.y;
            break;

        default:
            break;
    }
}
