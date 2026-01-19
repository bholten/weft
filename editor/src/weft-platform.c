#include <stdlib.h>

#include <SDL3/SDL.h>

#include "SDL3/SDL_video.h"
#include "weft.h"

/*
Intent of Platform:
Platform

SDL_Window* win;

display metrics (DPI scale, window size, etc.)

clipboard / IME helpers

RenderBackend

Pick one:

SDL_Renderer route (simpler)

SDL GPU route (more control)

Either way, render should accept “draw lists” (glyph quads + rectangles +
lines), not call into editor internals.
*/

struct platform {
  SDL_Window *window;
  SDL_Renderer *renderer;
  int width;
  int height;
  int dpi;
};

platform *weft_platform_new(int width, int height) {
  platform *p = calloc(1, sizeof(*p));

  if (!p) {
    return NULL;
  }

  SDL_Window *win =
      SDL_CreateWindow("weft", width, height, SDL_WINDOW_RESIZABLE);

  if (!win) {
    return NULL;
  }

  SDL_Renderer *renderer = SDL_CreateRenderer(win, NULL);

  if (!renderer) {
    return NULL;
  }

  p->window = win;
  p->renderer = renderer;
  p->width = width;
  p->height = height;
  p->dpi = 0; // TODO

  return p;
}
