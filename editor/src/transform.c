#include "transform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"
#include "document.h"

// === Transform Registry Entry ===

typedef struct TransformEntry {
  char *name;
  char *description;
  TransformFn fn;
  TransformCleanupFn cleanup;
  void *user_data;
  struct TransformEntry *next;
} TransformEntry;

// Global registry (simple linked list for now)
static TransformEntry *g_registry = NULL;
static bool g_initialized = false;

// === Registry Implementation ===

void transform_registry_init(void) {
  if (g_initialized) {
    return;
  }
  g_registry = NULL;
  g_initialized = true;
}

void transform_registry_cleanup(void) {
  TransformEntry *entry = g_registry;
  while (entry) {
    TransformEntry *next = entry->next;
    if (entry->cleanup) {
      entry->cleanup(entry->user_data);
    }
    free(entry->name);
    free(entry->description);
    free(entry);
    entry = next;
  }
  g_registry = NULL;
  g_initialized = false;
}

static TransformEntry *find_entry(const char *name) {
  for (TransformEntry *e = g_registry; e; e = e->next) {
    if (strcmp(e->name, name) == 0) {
      return e;
    }
  }
  return NULL;
}

bool transform_register(const char *name, const char *description,
                        TransformFn fn, TransformCleanupFn cleanup,
                        void *user_data) {
  if (!name || !fn) {
    return false;
  }

  // Check if already registered
  if (find_entry(name)) {
    return false;
  }

  TransformEntry *entry = calloc(1, sizeof(TransformEntry));
  if (!entry) {
    return false;
  }

  entry->name = strdup(name);
  entry->description = description ? strdup(description) : NULL;
  entry->fn = fn;
  entry->cleanup = cleanup;
  entry->user_data = user_data;

  // Add to front of list
  entry->next = g_registry;
  g_registry = entry;

  return true;
}

void transform_unregister(const char *name) {
  if (!name) {
    return;
  }

  TransformEntry **pp = &g_registry;
  while (*pp) {
    if (strcmp((*pp)->name, name) == 0) {
      TransformEntry *entry = *pp;
      *pp = entry->next;

      if (entry->cleanup) {
        entry->cleanup(entry->user_data);
      }
      free(entry->name);
      free(entry->description);
      free(entry);
      return;
    }
    pp = &(*pp)->next;
  }
}

TransformFn transform_lookup(const char *name) {
  TransformEntry *entry = find_entry(name);
  return entry ? entry->fn : NULL;
}

const char *transform_get_description(const char *name) {
  TransformEntry *entry = find_entry(name);
  return entry ? entry->description : NULL;
}

size_t transform_list(const char **out_names, size_t max_names) {
  size_t count = 0;
  for (TransformEntry *e = g_registry; e && count < max_names; e = e->next) {
    if (out_names) {
      out_names[count] = e->name;
    }
    count++;
  }
  return count;
}

Patch *transform_run(const char *name, Buffer *buffer, size_t start, size_t end,
                     const char **args) {
  TransformEntry *entry = find_entry(name);
  if (!entry) {
    return NULL;
  }
  return entry->fn(buffer, start, end, args, entry->user_data);
}

// === Built-in Transforms ===

// Helper: Get text from buffer range
static char *get_range_text(Buffer *buffer, size_t start, size_t end) {
  if (!buffer || !buffer->document || start >= end) {
    return NULL;
  }

  size_t len = end - start;
  char *text = malloc(len + 1);
  if (!text) {
    return NULL;
  }

  document_get_text(buffer->document, start, text, len);
  text[len] = '\0';
  return text;
}

Patch *transform_upper(Buffer *buffer, size_t start, size_t end,
                       const char **args, void *user_data) {
  (void)args;
  (void)user_data;

  char *text = get_range_text(buffer, start, end);
  if (!text) {
    return NULL;
  }

  // Convert to uppercase
  for (char *p = text; *p; p++) {
    *p = (char)toupper((unsigned char)*p);
  }

  Patch *patch = patch_new("upper");
  if (!patch) {
    free(text);
    return NULL;
  }

  patch_set_description(patch, "Convert text to uppercase");

  if (!patch_add(patch, start, end, text)) {
    free(text);
    patch_free(patch);
    return NULL;
  }

  free(text);
  return patch;
}

Patch *transform_lower(Buffer *buffer, size_t start, size_t end,
                       const char **args, void *user_data) {
  (void)args;
  (void)user_data;

  char *text = get_range_text(buffer, start, end);
  if (!text) {
    return NULL;
  }

  // Convert to lowercase
  for (char *p = text; *p; p++) {
    *p = (char)tolower((unsigned char)*p);
  }

  Patch *patch = patch_new("lower");
  if (!patch) {
    free(text);
    return NULL;
  }

  patch_set_description(patch, "Convert text to lowercase");

  if (!patch_add(patch, start, end, text)) {
    free(text);
    patch_free(patch);
    return NULL;
  }

  free(text);
  return patch;
}

// Helper: Compare lines for sorting
static int compare_lines(const void *a, const void *b) {
  const char *la = *(const char **)a;
  const char *lb = *(const char **)b;
  return strcmp(la, lb);
}

Patch *transform_sort_lines(Buffer *buffer, size_t start, size_t end,
                            const char **args, void *user_data) {
  (void)args;
  (void)user_data;

  char *text = get_range_text(buffer, start, end);
  if (!text) {
    return NULL;
  }

  size_t text_len = end - start;

  // Count lines
  size_t line_count = 1;
  for (char *p = text; *p; p++) {
    if (*p == '\n') {
      line_count++;
    }
  }

  // Build array of line pointers
  char **lines = malloc(line_count * sizeof(char *));
  if (!lines) {
    free(text);
    return NULL;
  }

  size_t line_idx = 0;
  lines[line_idx++] = text;
  for (char *p = text; *p; p++) {
    if (*p == '\n') {
      *p = '\0'; // Null-terminate this line
      if (*(p + 1)) {
        lines[line_idx++] = p + 1;
      }
    }
  }
  line_count = line_idx; // Actual count

  // Sort
  qsort(lines, line_count, sizeof(char *), compare_lines);

  // Build result string
  size_t result_len = text_len + 1;
  char *result = malloc(result_len);
  if (!result) {
    free(lines);
    free(text);
    return NULL;
  }

  size_t pos = 0;
  for (size_t i = 0; i < line_count; i++) {
    size_t len = strlen(lines[i]);
    memcpy(result + pos, lines[i], len);
    pos += len;
    if (i < line_count - 1) {
      result[pos++] = '\n';
    }
  }
  result[pos] = '\0';

  // If original ended with newline, add it back
  if (text_len > 0) {
    char last_char;
    document_get_text(buffer->document, end - 1, &last_char, 1);
    if (last_char == '\n' && pos > 0 && result[pos - 1] != '\n') {
      result[pos++] = '\n';
      result[pos] = '\0';
    }
  }

  free(lines);
  free(text);

  Patch *patch = patch_new("sort-lines");
  if (!patch) {
    free(result);
    return NULL;
  }

  patch_set_description(patch, "Sort lines alphabetically");

  if (!patch_add(patch, start, end, result)) {
    free(result);
    patch_free(patch);
    return NULL;
  }

  free(result);
  return patch;
}

Patch *transform_reverse_lines(Buffer *buffer, size_t start, size_t end,
                               const char **args, void *user_data) {
  (void)args;
  (void)user_data;

  char *text = get_range_text(buffer, start, end);
  if (!text) {
    return NULL;
  }

  size_t text_len = end - start;

  // Count lines and build array
  size_t line_count = 1;
  for (char *p = text; *p; p++) {
    if (*p == '\n') {
      line_count++;
    }
  }

  char **lines = malloc(line_count * sizeof(char *));
  if (!lines) {
    free(text);
    return NULL;
  }

  size_t line_idx = 0;
  lines[line_idx++] = text;
  for (char *p = text; *p; p++) {
    if (*p == '\n') {
      *p = '\0';
      if (*(p + 1)) {
        lines[line_idx++] = p + 1;
      }
    }
  }
  line_count = line_idx;

  // Build result in reverse order
  size_t result_len = text_len + 1;
  char *result = malloc(result_len);
  if (!result) {
    free(lines);
    free(text);
    return NULL;
  }

  size_t pos = 0;
  for (size_t i = line_count; i > 0; i--) {
    size_t len = strlen(lines[i - 1]);
    memcpy(result + pos, lines[i - 1], len);
    pos += len;
    if (i > 1) {
      result[pos++] = '\n';
    }
  }
  result[pos] = '\0';

  // Preserve trailing newline if original had one
  if (text_len > 0) {
    char last_char;
    document_get_text(buffer->document, end - 1, &last_char, 1);
    if (last_char == '\n' && pos > 0 && result[pos - 1] != '\n') {
      result[pos++] = '\n';
      result[pos] = '\0';
    }
  }

  free(lines);
  free(text);

  Patch *patch = patch_new("reverse-lines");
  if (!patch) {
    free(result);
    return NULL;
  }

  patch_set_description(patch, "Reverse line order");

  if (!patch_add(patch, start, end, result)) {
    free(result);
    patch_free(patch);
    return NULL;
  }

  free(result);
  return patch;
}

void transform_register_builtins(void) {
  transform_register("upper", "Convert text to UPPERCASE", transform_upper,
                     NULL, NULL);
  transform_register("lower", "Convert text to lowercase", transform_lower,
                     NULL, NULL);
  transform_register("sort-lines", "Sort lines alphabetically",
                     transform_sort_lines, NULL, NULL);
  transform_register("reverse-lines", "Reverse line order",
                     transform_reverse_lines, NULL, NULL);
}

// === Lcl-Driven Transforms ===

// Lcl transform user data
typedef struct {
  LclHost *host;
  char *script;
} LclTransformData;

static void lcl_transform_cleanup(void *user_data) {
  LclTransformData *data = user_data;
  if (data) {
    free(data->script);
    free(data);
  }
}

// Note: This is a placeholder - actual Lcl integration requires LclHost access
// and proper evaluation of the script
static Patch *lcl_transform_fn(Buffer *buffer, size_t start, size_t end,
                               const char **args, void *user_data) {
  (void)buffer;
  (void)start;
  (void)end;
  (void)args;
  (void)user_data;

  // TODO: Implement Lcl script execution
  // The script should receive $start, $end, $text and return a patch structure
  fprintf(stderr, "Lcl transforms not yet implemented\n");
  return NULL;
}

bool transform_register_lcl(const char *name, const char *description,
                            LclHost *host, const char *script) {
  if (!name || !host || !script) {
    return false;
  }

  LclTransformData *data = calloc(1, sizeof(LclTransformData));
  if (!data) {
    return false;
  }

  data->host = host;
  data->script = strdup(script);
  if (!data->script) {
    free(data);
    return false;
  }

  if (!transform_register(name, description, lcl_transform_fn,
                          lcl_transform_cleanup, data)) {
    lcl_transform_cleanup(data);
    return false;
  }

  return true;
}
