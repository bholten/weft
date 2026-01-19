#include "renderer.h"

#include <stdlib.h>
#include <string.h>

Renderer *renderer_new(Platform *p) {
  if (!p) {
    return NULL;
  }

  if (!TTF_Init()) {
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "TTF_Init failed: %s",
                 SDL_GetError());
    return NULL;
  }

  Renderer *r = calloc(1, sizeof(Renderer));
  if (!r) {
    return NULL;
  }

  r->platform = p;
  r->font = NULL;

  return r;
}

void renderer_free(Renderer *r) {
  if (!r) {
    return;
  }

  if (r->font) {
    if (r->font->font) {
      TTF_CloseFont(r->font->font);
    }
    free(r->font);
  }

  TTF_Quit();
  free(r);
}

bool renderer_load_font(Renderer *r, const char *path, int size) {
  if (!r || !path) {
    return false;
  }

  // Scale font size by DPI for crisp rendering on high-DPI displays
  float dpi_scale = platform_get_dpi_scale(r->platform);
  int scaled_size = (int)(size * dpi_scale);

  TTF_Font *ttf_font = TTF_OpenFont(path, scaled_size);
  if (!ttf_font) {
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "TTF_OpenFont failed: %s",
                 SDL_GetError());
    return false;
  }

  // Free existing font if any
  if (r->font) {
    if (r->font->font) {
      TTF_CloseFont(r->font->font);
    }
    free(r->font);
  }

  r->font = calloc(1, sizeof(Font));
  if (!r->font) {
    TTF_CloseFont(ttf_font);
    return false;
  }

  r->font->font = ttf_font;
  r->font->font_size = size;
  r->font->line_height = TTF_GetFontLineSkip(ttf_font);

  // Measure char width for monospace (use 'M' as reference)
  int advance = 0;
  if (TTF_GetGlyphMetrics(ttf_font, 'M', NULL, NULL, NULL, NULL, &advance)) {
    r->font->char_width = advance;
  } else {
    // Fallback: estimate based on font size
    r->font->char_width = size;
  }

  return true;
}

void renderer_clear(Renderer *r, uint8_t red, uint8_t green, uint8_t blue) {
  if (!r || !r->platform) {
    return;
  }

  SDL_Renderer *sdl_renderer = platform_get_sdl_renderer(r->platform);
  if (!sdl_renderer) {
    return;
  }

  SDL_SetRenderDrawColor(sdl_renderer, red, green, blue, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(sdl_renderer);
}

void renderer_draw_text(Renderer *r, const char *text, int x, int y,
                        SDL_Color color) {
  if (!text) {
    return;
  }
  renderer_draw_text_n(r, text, strlen(text), x, y, color);
}

void renderer_draw_text_n(Renderer *r, const char *text, size_t len, int x,
                          int y, SDL_Color color) {
  if (!r || !r->platform || !r->font || !r->font->font || !text || len == 0) {
    return;
  }

  SDL_Renderer *sdl_renderer = platform_get_sdl_renderer(r->platform);
  if (!sdl_renderer) {
    return;
  }

  SDL_Surface *surface =
      TTF_RenderText_Blended(r->font->font, text, len, color);
  if (!surface) {
    return;
  }

  SDL_Texture *texture = SDL_CreateTextureFromSurface(sdl_renderer, surface);
  SDL_DestroySurface(surface);

  if (!texture) {
    return;
  }

  SDL_PropertiesID props = SDL_GetTextureProperties(texture);
  SDL_FRect dest = {.x = (float)x,
                    .y = (float)y,
                    .w = (float)SDL_GetNumberProperty(
                        props, SDL_PROP_TEXTURE_WIDTH_NUMBER, 0),
                    .h = (float)SDL_GetNumberProperty(
                        props, SDL_PROP_TEXTURE_HEIGHT_NUMBER, 0)};

  SDL_RenderTexture(sdl_renderer, texture, NULL, &dest);
  SDL_DestroyTexture(texture);
}

void renderer_draw_rect(Renderer *r, int x, int y, int w, int h,
                        SDL_Color color) {
  if (!r || !r->platform) {
    return;
  }

  SDL_Renderer *sdl_renderer = platform_get_sdl_renderer(r->platform);
  if (!sdl_renderer) {
    return;
  }

  SDL_SetRenderDrawColor(sdl_renderer, color.r, color.g, color.b, color.a);

  SDL_FRect rect = {.x = (float)x, .y = (float)y, .w = (float)w, .h = (float)h};

  SDL_RenderFillRect(sdl_renderer, &rect);
}

void renderer_draw_line(Renderer *r, int x1, int y1, int x2, int y2,
                        SDL_Color color) {
  if (!r || !r->platform) {
    return;
  }

  SDL_Renderer *sdl_renderer = platform_get_sdl_renderer(r->platform);
  if (!sdl_renderer) {
    return;
  }

  SDL_SetRenderDrawColor(sdl_renderer, color.r, color.g, color.b, color.a);
  SDL_RenderLine(sdl_renderer, (float)x1, (float)y1, (float)x2, (float)y2);
}

void renderer_present(Renderer *r) {
  if (!r || !r->platform) {
    return;
  }

  SDL_Renderer *sdl_renderer = platform_get_sdl_renderer(r->platform);
  if (!sdl_renderer) {
    return;
  }

  SDL_RenderPresent(sdl_renderer);
}

int renderer_get_line_height(Renderer *r) {
  if (!r || !r->font) {
    return 0;
  }
  return r->font->line_height;
}

int renderer_get_char_width(Renderer *r) {
  if (!r || !r->font) {
    return 0;
  }
  return r->font->char_width;
}
