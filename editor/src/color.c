#include "color.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

bool color_parse_hex(const char *str, SDL_Color *out) {
  if (!str || str[0] != '#') {
    return false;
  }

  size_t len = strlen(str);
  unsigned int r;
  unsigned int g;
  unsigned int b;

  if (len == 7) {
    // #RRGGBB
    if (sscanf(str, "#%02x%02x%02x", &r, &g, &b) == 3) {
      out->r = (uint8_t)r;
      out->g = (uint8_t)g;
      out->b = (uint8_t)b;
      out->a = 255;
      return true;
    }
  } else if (len == 4) {
    // #RGB -> #RRGGBB
    if (sscanf(str, "#%1x%1x%1x", &r, &g, &b) == 3) {
      out->r = (uint8_t)(r * 17);
      out->g = (uint8_t)(g * 17);
      out->b = (uint8_t)(b * 17);
      out->a = 255;
      return true;
    }
  }

  return false;
}

bool color_parse_named(const char *str, SDL_Color *out) {
  if (!str) {
    return false;
  }

  // Basic named colors
  struct {
    const char *name;
    uint8_t r, g, b;
  } colors[] = {
      {"red",     255, 0,   0  },
      {"green",   0,   255, 0  },
      {"blue",    0,   0,   255},
      {"yellow",  255, 255, 0  },
      {"cyan",    0,   255, 255},
      {"magenta", 255, 0,   255},
      {"white",   255, 255, 255},
      {"black",   0,   0,   0  },
      {"orange",  255, 165, 0  },
      {"purple",  128, 0,   128},
      {"pink",    255, 192, 203},
      {"gray",    128, 128, 128},
      {"grey",    128, 128, 128},
  };

  for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
    if (strcasecmp(str, colors[i].name) == 0) {
      out->r = colors[i].r;
      out->g = colors[i].g;
      out->b = colors[i].b;
      out->a = 255;
      return true;
    }
  }

  return false;
}

bool color_parse(const char *str, SDL_Color *out) {
  return color_parse_hex(str, out) || color_parse_named(str, out);
}
