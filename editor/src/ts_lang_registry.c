#include "ts_lang_registry.h"
#include "tree_sitter_hl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Platform-specific dynamic loading
#ifdef _WIN32
#include <windows.h>
#define DL_OPEN(path) LoadLibraryA(path)
#define DL_SYM(handle, name) (void *)GetProcAddress((HMODULE)(handle), (name))
#define DL_CLOSE(handle) FreeLibrary((HMODULE)(handle))
#define DL_ERROR() "LoadLibrary failed"
#else
#include <dlfcn.h>
#define DL_OPEN(path) dlopen((path), RTLD_LAZY)
#define DL_SYM(handle, name) dlsym((handle), (name))
#define DL_CLOSE(handle) dlclose(handle)
#define DL_ERROR() dlerror()
#endif

// Initial capacity for arrays
#define INITIAL_CAP 8

// Helper: Read file contents
static char *read_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *content = malloc(size + 1);
  if (!content) {
    fclose(f);
    return NULL;
  }

  size_t read = fread(content, 1, size, f);
  fclose(f);
  content[read] = '\0';

  return content;
}

// Helper: Build function name from language name
// e.g., "c" -> "tree_sitter_c", "javascript" -> "tree_sitter_javascript"
static char *build_func_name(const char *lang_name) {
  size_t len = strlen("tree_sitter_") + strlen(lang_name) + 1;
  char *func_name = malloc(len);
  if (!func_name) {
    return NULL;
  }
  snprintf(func_name, len, "tree_sitter_%s", lang_name);
  return func_name;
}

// Helper: Build default query path from library path
// e.g., "/path/to/libtree-sitter-c.so" -> "/path/to/queries/highlights.scm"
static char *build_default_query_path(const char *library_path) {
  // Find last directory separator
  const char *last_sep = strrchr(library_path, '/');
#ifdef _WIN32
  const char *last_sep_win = strrchr(library_path, '\\');
  if (last_sep_win > last_sep) {
    last_sep = last_sep_win;
  }
#endif

  size_t dir_len;

  if (last_sep) {
    dir_len = last_sep - library_path;
  } else {
    dir_len = 0;
  }

  // Build path: <dir>/queries/highlights.scm
  const char *suffix = "/queries/highlights.scm";
  size_t path_len = dir_len + strlen(suffix) + 1;
  char *path = malloc(path_len);

  if (!path) {
    return NULL;
  }

  if (dir_len > 0) {
    memcpy(path, library_path, dir_len);
    path[dir_len] = '\0';
  } else {
    path[0] = '\0';
  }
  strcat(path, suffix);

  return path;
}

TSLangRegistry *ts_lang_registry_new(void) {
  TSLangRegistry *reg = calloc(1, sizeof(TSLangRegistry));
  if (!reg) {
    return NULL;
  }

  reg->languages = calloc(INITIAL_CAP, sizeof(TSLangConfig));
  reg->language_cap = INITIAL_CAP;

  reg->mappings = calloc(INITIAL_CAP, sizeof(TSExtMapping));
  reg->mapping_cap = INITIAL_CAP;

  if (!reg->languages || !reg->mappings) {
    free(reg->languages);
    free(reg->mappings);
    free(reg);
    return NULL;
  }

  return reg;
}

static void lang_config_free(TSLangConfig *config) {
  if (!config) {
    return;
  }

  free(config->name);
  free(config->library_path);
  free(config->query_path);

  if (config->highlighter) {
    ts_hl_free(config->highlighter);
  }

  if (config->dl_handle) {
    DL_CLOSE(config->dl_handle);
  }

  // Note: config->language is owned by the shared library, don't free
}

void ts_lang_registry_free(TSLangRegistry *reg) {
  if (!reg) {
    return;
  }

  for (size_t i = 0; i < reg->language_count; i++) {
    lang_config_free(&reg->languages[i]);
  }

  free(reg->languages);

  for (size_t i = 0; i < reg->mapping_count; i++) {
    free(reg->mappings[i].extension);
    free(reg->mappings[i].language_name);
  }

  free(reg->mappings);

  free(reg);
}

bool ts_lang_registry_add_dynamic(TSLangRegistry *reg, const char *name,
                                  const char *library_path,
                                  const char *query_path) {
  if (!reg || !name || !library_path) {
    return false;
  }

  // Check if already registered
  for (size_t i = 0; i < reg->language_count; i++) {
    if (strcmp(reg->languages[i].name, name) == 0) {
      fprintf(stderr, "ts_lang_registry: language '%s' already registered\n",
              name);
      return false;
    }
  }

  // Grow array if needed
  if (reg->language_count >= reg->language_cap) {
    size_t new_cap = reg->language_cap * 2;
    TSLangConfig *new_langs =
        realloc(reg->languages, new_cap * sizeof(TSLangConfig));
    if (!new_langs) {
      return false;
    }
    reg->languages = new_langs;
    reg->language_cap = new_cap;
  }

  TSLangConfig *config = &reg->languages[reg->language_count];
  memset(config, 0, sizeof(TSLangConfig));

  config->name = strdup(name);
  config->library_path = strdup(library_path);
  config->query_path =
      query_path ? strdup(query_path) : build_default_query_path(library_path);
  config->source = TS_LANG_SOURCE_DYNAMIC;

  if (!config->name || !config->library_path || !config->query_path) {
    free(config->name);
    free(config->library_path);
    free(config->query_path);
    return false;
  }

  reg->language_count++;
  return true;
}

bool ts_lang_registry_add_embedded(TSLangRegistry *reg, const char *name,
                                   TSLangFunc func, const char *query_source,
                                   const char *query_path) {
  if (!reg || !name || !func) {
    return false;
  }

  // Must have either query_source or query_path
  if (!query_source && !query_path) {
    fprintf(stderr,
            "ts_lang_registry: embedded language '%s' needs query_source or "
            "query_path\n",
            name);
    return false;
  }

  // Check if already registered
  for (size_t i = 0; i < reg->language_count; i++) {
    if (strcmp(reg->languages[i].name, name) == 0) {
      fprintf(stderr, "ts_lang_registry: language '%s' already registered\n",
              name);
      return false;
    }
  }

  // Grow array if needed
  if (reg->language_count >= reg->language_cap) {
    size_t new_cap = reg->language_cap * 2;
    TSLangConfig *new_langs =
        realloc(reg->languages, new_cap * sizeof(TSLangConfig));
    if (!new_langs) {
      return false;
    }
    reg->languages = new_langs;
    reg->language_cap = new_cap;
  }

  TSLangConfig *config = &reg->languages[reg->language_count];
  memset(config, 0, sizeof(TSLangConfig));

  config->name = strdup(name);
  config->source = TS_LANG_SOURCE_EMBEDDED;
  config->embedded_func = func;
  config->language = func(); // Get language immediately for embedded

  // For embedded, we might have query_source as a string or need to load from
  // file
  if (query_source) {
    // Store query_path as the source itself (we'll handle this specially)
    config->query_path = strdup(query_source);
  } else {
    config->query_path = strdup(query_path);
  }

  if (!config->name || !config->query_path) {
    free(config->name);
    free(config->query_path);
    return false;
  }

  reg->language_count++;
  return true;
}

bool ts_lang_registry_map_ext(TSLangRegistry *reg, const char *extension,
                              const char *language_name) {
  if (!reg || !extension || !language_name) {
    return false;
  }

  // Check if extension already mapped (update if so)
  for (size_t i = 0; i < reg->mapping_count; i++) {
    if (strcmp(reg->mappings[i].extension, extension) == 0) {
      free(reg->mappings[i].language_name);
      reg->mappings[i].language_name = strdup(language_name);
      return reg->mappings[i].language_name != NULL;
    }
  }

  // Grow array if needed
  if (reg->mapping_count >= reg->mapping_cap) {
    size_t new_cap = reg->mapping_cap * 2;
    TSExtMapping *new_mappings =
        realloc(reg->mappings, new_cap * sizeof(TSExtMapping));
    if (!new_mappings) {
      return false;
    }
    reg->mappings = new_mappings;
    reg->mapping_cap = new_cap;
  }

  TSExtMapping *mapping = &reg->mappings[reg->mapping_count];
  mapping->extension = strdup(extension);
  mapping->language_name = strdup(language_name);

  if (!mapping->extension || !mapping->language_name) {
    free(mapping->extension);
    free(mapping->language_name);
    return false;
  }

  reg->mapping_count++;
  return true;
}

TSLangConfig *ts_lang_registry_get(TSLangRegistry *reg, const char *name) {
  if (!reg || !name) {
    return NULL;
  }

  for (size_t i = 0; i < reg->language_count; i++) {
    if (strcmp(reg->languages[i].name, name) == 0) {
      return &reg->languages[i];
    }
  }

  return NULL;
}

TSLangConfig *ts_lang_registry_for_ext(TSLangRegistry *reg,
                                       const char *extension) {
  if (!reg || !extension) {
    return NULL;
  }

  // Find mapping for extension
  for (size_t i = 0; i < reg->mapping_count; i++) {
    if (strcmp(reg->mappings[i].extension, extension) == 0) {
      return ts_lang_registry_get(reg, reg->mappings[i].language_name);
    }
  }

  return NULL;
}

// Load the TSLanguage for a config (if not already loaded)
static bool load_language(TSLangConfig *config) {
  if (config->load_attempted) {
    return !config->load_failed;
  }

  config->load_attempted = true;

  if (config->source == TS_LANG_SOURCE_EMBEDDED) {
    // Already loaded in add_embedded
    if (!config->language) {
      config->language = config->embedded_func();
    }
    config->load_failed = (config->language == NULL);
    return !config->load_failed;
  }

  // Dynamic loading
  config->dl_handle = DL_OPEN(config->library_path);
  if (!config->dl_handle) {
    fprintf(stderr, "ts_lang_registry: failed to load '%s': %s\n",
            config->library_path, DL_ERROR());
    config->load_failed = true;
    return false;
  }

  // Build function name and look it up
  char *func_name = build_func_name(config->name);
  if (!func_name) {
    config->load_failed = true;
    return false;
  }

  TSLangFunc func = (TSLangFunc)DL_SYM(config->dl_handle, func_name);
  if (!func) {
    fprintf(stderr, "ts_lang_registry: symbol '%s' not found in '%s': %s\n",
            func_name, config->library_path, DL_ERROR());
    free(func_name);
    config->load_failed = true;
    return false;
  }

  free(func_name);

  config->language = func();

  if (!config->language) {
    fprintf(stderr, "ts_lang_registry: %s() returned NULL\n", config->name);
    config->load_failed = true;
    return false;
  }

  return true;
}

TSHighlighter *ts_lang_registry_get_highlighter(TSLangRegistry *reg,
                                                TSLangConfig *config) {
  if (!reg || !config) {
    return NULL;
  }

  // Return cached highlighter if available
  if (config->highlighter) {
    return config->highlighter;
  }

  // Don't retry if already failed
  if (config->load_failed) {
    return NULL;
  }

  // Load the language if needed
  if (!load_language(config)) {
    return NULL;
  }

  // Load query source
  char *query_source = NULL;

  if (config->source == TS_LANG_SOURCE_EMBEDDED && config->query_path) {
    // For embedded, query_path might be the actual query source
    // Check if it looks like a file path or query content
    if (config->query_path[0] == '(' || config->query_path[0] == ';' ||
        config->query_path[0] == '[') {
      // Looks like query content
      query_source = strdup(config->query_path);
    } else {
      // Treat as file path
      query_source = read_file(config->query_path);
    }
  } else {
    query_source = read_file(config->query_path);
  }

  if (!query_source) {
    fprintf(stderr, "ts_lang_registry: failed to load query from '%s'\n",
            config->query_path);
    config->load_failed = true;
    return NULL;
  }

  // Create highlighter
  config->highlighter = ts_hl_new(config->language, config->name, query_source);
  free(query_source);

  if (!config->highlighter) {
    fprintf(stderr, "ts_lang_registry: failed to create highlighter for '%s'\n",
            config->name);
    config->load_failed = true;
    return NULL;
  }

  return config->highlighter;
}

const char **ts_lang_registry_list(TSLangRegistry *reg, size_t *count) {
  if (!reg || !count) {
    return NULL;
  }

  *count = reg->language_count;
  if (reg->language_count == 0) {
    return NULL;
  }

  const char **names = malloc(reg->language_count * sizeof(char *));

  if (!names) {
    return NULL;
  }

  for (size_t i = 0; i < reg->language_count; i++) {
    names[i] = reg->languages[i].name;
  }

  return names;
}
