#ifndef WEFT_TS_LANG_REGISTRY_H
#define WEFT_TS_LANG_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

// Forward declarations
typedef struct TSLanguage TSLanguage;
typedef struct TSHighlighter TSHighlighter;

// Language source type
typedef enum {
  TS_LANG_SOURCE_DYNAMIC, // Load from shared library at runtime
  TS_LANG_SOURCE_EMBEDDED // Use compiled-in parser
} TSLangSource;

// Embedded language function pointer type
typedef const TSLanguage *(*TSLangFunc)(void);

// Language configuration
typedef struct {
  char *name;               // Language name (e.g., "c", "javascript")
  char *library_path;       // Path to shared library (for dynamic loading)
  char *query_path;         // Path to highlights.scm
  TSLangSource source;      // How to load the language
  TSLangFunc embedded_func; // Function pointer for embedded languages

  // Cached state (lazily loaded)
  void *dl_handle;            // dlopen handle (for dynamic)
  const TSLanguage *language; // Loaded language
  TSHighlighter *highlighter; // Created highlighter
  bool load_attempted;        // Whether we've tried to load
  bool load_failed;           // Whether loading failed
} TSLangConfig;

// Extension to language mapping
typedef struct {
  char *extension;     // File extension (e.g., ".c", ".js")
  char *language_name; // Language name to use
} TSExtMapping;

// Language registry
typedef struct TSLangRegistry {
  TSLangConfig *languages;
  size_t language_count;
  size_t language_cap;

  TSExtMapping *mappings;
  size_t mapping_count;
  size_t mapping_cap;
} TSLangRegistry;

// Create/destroy registry
TSLangRegistry *ts_lang_registry_new(void);
void ts_lang_registry_free(TSLangRegistry *reg);

// Register a dynamic language (loaded from shared library)
// library_path: path to .so/.dylib/.dll
// query_path: path to highlights.scm (or NULL to look in same dir as library)
bool ts_lang_registry_add_dynamic(TSLangRegistry *reg, const char *name,
                                  const char *library_path,
                                  const char *query_path);

// Register an embedded language (compiled into binary)
// func: function that returns the TSLanguage (e.g., tree_sitter_c)
// query_source: the highlights.scm content as a string (can be NULL)
// query_path: path to highlights.scm file (used if query_source is NULL)
bool ts_lang_registry_add_embedded(TSLangRegistry *reg, const char *name,
                                   TSLangFunc func, const char *query_source,
                                   const char *query_path);

// Map file extension to language
bool ts_lang_registry_map_ext(TSLangRegistry *reg, const char *extension,
                              const char *language_name);

// Get language config by name
TSLangConfig *ts_lang_registry_get(TSLangRegistry *reg, const char *name);

// Get language config for a file extension
TSLangConfig *ts_lang_registry_for_ext(TSLangRegistry *reg,
                                       const char *extension);

// Get or create highlighter for a language config
// Returns NULL if loading fails
TSHighlighter *ts_lang_registry_get_highlighter(TSLangRegistry *reg,
                                                TSLangConfig *config);

// List registered languages (returns array of names, caller frees array but not
// strings)
const char **ts_lang_registry_list(TSLangRegistry *reg, size_t *count);

#endif // WEFT_TS_LANG_REGISTRY_H
