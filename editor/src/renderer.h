#ifndef WEFT_RENDERER_H
#define WEFT_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "platform.h"

typedef struct {
  TTF_Font *font;
  int font_size;
  int line_height;
  int char_width; // For monospace fonts
} Font;

typedef struct Renderer {
  Platform *platform;
  Font *font;
} Renderer;

// Lifecycle
Renderer *renderer_new(Platform *p);
void renderer_free(Renderer *r);

// Font management
bool renderer_load_font(Renderer *r, const char *path, int size);

// Drawing operations
void renderer_clear(Renderer *r, uint8_t red, uint8_t green, uint8_t blue);
void renderer_draw_text(Renderer *r, const char *text, int x, int y,
                        SDL_Color color);
void renderer_draw_text_n(Renderer *r, const char *text, size_t len, int x,
                          int y, SDL_Color color);
void renderer_draw_rect(Renderer *r, int x, int y, int w, int h,
                        SDL_Color color);
void renderer_draw_line(Renderer *r, int x1, int y1, int x2, int y2,
                        SDL_Color color);
void renderer_present(Renderer *r);

// Metrics
int renderer_get_line_height(Renderer *r);
int renderer_get_char_width(Renderer *r);

#endif // WEFT_RENDERER_H
