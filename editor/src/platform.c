#include "platform.h"
#include "SDL3/SDL_video.h"

#include <stdlib.h>

Platform *platform_new(const char *title, int width, int height) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "SDL_Init failed: %s",
                 SDL_GetError());
    return NULL;
  }

  // Create window hidden initially to avoid flicker during positioning
  SDL_Window *window =
      SDL_CreateWindow(title, width, height,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                           SDL_WINDOW_HIDDEN);

  if (!window) {
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "SDL_CreateWindow failed: %s",
                 SDL_GetError());
    SDL_Quit();
    return NULL;
  }

  // Position window centered before showing
  SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  SDL_ShowWindow(window);

  SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);

  if (!renderer) {
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "SDL_CreateRenderer failed: %s",
                 SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return NULL;
  }

  Platform *p = calloc(1, sizeof(Platform));

  if (!p) {
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return NULL;
  }

  p->window = window;
  p->sdl_renderer = renderer;
  p->width = width;
  p->height = height;
  p->dpi_scale = SDL_GetWindowDisplayScale(window);

  return p;
}

void platform_free(Platform *p) {
  if (!p) {
    return;
  }

  if (p->sdl_renderer) {
    SDL_DestroyRenderer(p->sdl_renderer);
  }

  if (p->window) {
    SDL_DestroyWindow(p->window);
  }

  free(p);
  SDL_Quit();
}

void platform_get_size(Platform *p, int *w, int *h) {
  if (!p) {
    if (w) {
      *w = 0;
    }
    if (h) {
      *h = 0;
    }
    return;
  }

  if (w) {
    *w = p->width;
  }
  if (h) {
    *h = p->height;
  }
}

float platform_get_dpi_scale(Platform *p) {
  if (!p) {
    return 1.0f;
  }
  return p->dpi_scale;
}

SDL_Renderer *platform_get_sdl_renderer(Platform *p) {
  if (!p) {
    return NULL;
  }
  return p->sdl_renderer;
}

SDL_Window *platform_get_window(Platform *p) {
  if (!p) {
    return NULL;
  }
  return p->window;
}

void platform_update_size(Platform *p) {
  if (!p || !p->window) {
    return;
  }

  SDL_GetWindowSize(p->window, &p->width, &p->height);
  p->dpi_scale = SDL_GetWindowDisplayScale(p->window);
}
