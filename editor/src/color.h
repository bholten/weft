#ifndef WEFT_COLOR_H
#define WEFT_COLOR_H

#include <SDL3/SDL.h>
#include <stdbool.h>

// Parse a hex color string (#RGB or #RRGGBB)
bool color_parse_hex(const char *str, SDL_Color *out);

// Parse a named color (red, green, blue, etc.)
bool color_parse_named(const char *str, SDL_Color *out);

// Parse any color value (hex or named)
bool color_parse(const char *str, SDL_Color *out);

#endif // WEFT_COLOR_H
