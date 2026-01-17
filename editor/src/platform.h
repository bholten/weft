#ifndef WEFT_PLATFORM_H
#define WEFT_PLATFORM_H

#include <SDL3/SDL.h>

typedef struct Platform {
    SDL_Window *window;
    SDL_Renderer *sdl_renderer;
    int width;
    int height;
    float dpi_scale;
} Platform;

// Lifecycle
Platform *platform_new(const char *title, int width, int height);
void platform_free(Platform *p);

// Accessors
void platform_get_size(Platform *p, int *w, int *h);
float platform_get_dpi_scale(Platform *p);
SDL_Renderer *platform_get_sdl_renderer(Platform *p);
SDL_Window *platform_get_window(Platform *p);

// Update size (call on window resize)
void platform_update_size(Platform *p);

#endif // WEFT_PLATFORM_H
