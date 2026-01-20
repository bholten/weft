#include "transform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"
#include "document.h"
#include "editor.h"
#include "lcl_host.h"

#include <lcl.h>

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
  char *name;
  char *script;
} LclTransformData;

static void lcl_transform_cleanup(void *user_data) {
  LclTransformData *data = user_data;
  if (data) {
    free(data->name);
    free(data->script);
    free(data);
  }
}

// Parse a tagged return value from Lcl transform
// Expected formats:
//   {replace <text>}
//   {patch {{s1 e1 t1} {s2 e2 t2} ...}}
//   {cancel}
//   {prompt <message> ?<default>?}
// Returns a Patch on success, or NULL on cancel/error
// Sets *out_prompt_msg if the transform requests a prompt
static Patch *parse_lcl_transform_result(lcl_value *result, const char *name,
                                         size_t start, size_t end,
                                         char **out_prompt_msg,
                                         char **out_prompt_default) {
  if (!result) {
    return NULL;
  }

  // Result should be a list with tag as first element
  if (lcl_value_type_of(result) != LCL_LIST || lcl_list_len(result) < 1) {
    // Simple string result - treat as replacement text (Tier 1 shortcut)
    const char *text = lcl_value_to_string(result);
    if (!text) {
      return NULL;
    }

    Patch *patch = patch_new(name);
    if (!patch) {
      return NULL;
    }
    patch_set_description(patch, "Lcl transform");
    if (!patch_add(patch, start, end, text)) {
      patch_free(patch);
      return NULL;
    }
    return patch;
  }

  // Get the tag
  lcl_value *tag_val = NULL;
  if (lcl_list_get(result, 0, &tag_val) != LCL_OK) {
    return NULL;
  }
  const char *tag = lcl_value_to_string(tag_val);
  lcl_ref_dec(tag_val);

  if (!tag) {
    return NULL;
  }

  // Handle each tag type
  if (strcmp(tag, "replace") == 0) {
    // {replace <text>}
    if (lcl_list_len(result) < 2) {
      fprintf(stderr, "transform: replace requires text argument\n");
      return NULL;
    }

    lcl_value *text_val = NULL;
    if (lcl_list_get(result, 1, &text_val) != LCL_OK) {
      return NULL;
    }
    const char *text = lcl_value_to_string(text_val);
    lcl_ref_dec(text_val);

    if (!text) {
      return NULL;
    }

    Patch *patch = patch_new(name);
    if (!patch) {
      return NULL;
    }
    patch_set_description(patch, "Lcl transform (replace)");
    if (!patch_add(patch, start, end, text)) {
      patch_free(patch);
      return NULL;
    }
    return patch;

  } else if (strcmp(tag, "patch") == 0) {
    // {patch {{s1 e1 t1} {s2 e2 t2} ...}}
    if (lcl_list_len(result) < 2) {
      fprintf(stderr, "transform: patch requires edit list argument\n");
      return NULL;
    }

    lcl_value *edits_val = NULL;
    if (lcl_list_get(result, 1, &edits_val) != LCL_OK) {
      return NULL;
    }

    if (lcl_value_type_of(edits_val) != LCL_LIST) {
      lcl_ref_dec(edits_val);
      fprintf(stderr, "transform: patch argument must be a list\n");
      return NULL;
    }

    Patch *patch = patch_new(name);
    if (!patch) {
      lcl_ref_dec(edits_val);
      return NULL;
    }
    patch_set_description(patch, "Lcl transform (patch)");

    size_t edit_count = lcl_list_len(edits_val);
    for (size_t i = 0; i < edit_count; i++) {
      lcl_value *edit = NULL;
      if (lcl_list_get(edits_val, i, &edit) != LCL_OK) {
        continue;
      }

      if (lcl_value_type_of(edit) != LCL_LIST || lcl_list_len(edit) < 3) {
        lcl_ref_dec(edit);
        fprintf(stderr, "transform: each edit must be {start end text}\n");
        continue;
      }

      lcl_value *s_val = NULL, *e_val = NULL, *t_val = NULL;
      lcl_list_get(edit, 0, &s_val);
      lcl_list_get(edit, 1, &e_val);
      lcl_list_get(edit, 2, &t_val);

      long s = 0, e = 0;
      lcl_value_to_int(s_val, &s);
      lcl_value_to_int(e_val, &e);
      const char *t = lcl_value_to_string(t_val);

      if (s >= 0 && e >= s && t) {
        patch_add(patch, (size_t)s, (size_t)e, t);
      }

      if (s_val) {
        lcl_ref_dec(s_val);
      }
      if (e_val) {
        lcl_ref_dec(e_val);
      }
      if (t_val) {
        lcl_ref_dec(t_val);
      }
      lcl_ref_dec(edit);
    }

    lcl_ref_dec(edits_val);
    return patch;

  } else if (strcmp(tag, "cancel") == 0) {
    // {cancel} - transform explicitly cancelled
    return NULL;

  } else if (strcmp(tag, "prompt") == 0) {
    // {prompt <message> ?<default>?}
    if (lcl_list_len(result) < 2) {
      fprintf(stderr, "transform: prompt requires message argument\n");
      return NULL;
    }

    lcl_value *msg_val = NULL;
    if (lcl_list_get(result, 1, &msg_val) != LCL_OK) {
      return NULL;
    }
    const char *msg = lcl_value_to_string(msg_val);
    if (out_prompt_msg && msg) {
      *out_prompt_msg = strdup(msg);
    }
    lcl_ref_dec(msg_val);

    // Optional default value
    if (lcl_list_len(result) >= 3 && out_prompt_default) {
      lcl_value *def_val = NULL;
      if (lcl_list_get(result, 2, &def_val) == LCL_OK) {
        const char *def = lcl_value_to_string(def_val);
        if (def) {
          *out_prompt_default = strdup(def);
        }
        lcl_ref_dec(def_val);
      }
    }

    return NULL; // Prompt needs UI interaction before continuing
  }

  // Unknown tag
  fprintf(stderr, "transform: unknown tag '%s'\n", tag);
  return NULL;
}

// Execute Lcl transform script
static Patch *lcl_transform_fn(Buffer *buffer, size_t start, size_t end,
                               const char **args, void *user_data) {
  LclTransformData *data = user_data;
  if (!data || !data->host || !data->script) {
    return NULL;
  }

  LclHost *host = data->host;
  lcl_interp *interp = host->interp;

  // Get the text in the range
  char *text = NULL;
  if (buffer && buffer->document && end > start) {
    size_t len = end - start;
    text = malloc(len + 1);
    if (text) {
      document_get_text(buffer->document, start, text, len);
      text[len] = '\0';
    }
  }

  // Set up context variables
  lcl_define_take(interp, "text",
                  text ? lcl_string_new(text) : lcl_string_new(""));
  lcl_define_take(interp, "start", lcl_int_new((long)start));
  lcl_define_take(interp, "end", lcl_int_new((long)end));

  // File type from buffer path
  const char *filetype = "";
  if (buffer && buffer->file_path) {
    const char *ext = strrchr(buffer->file_path, '.');
    if (ext) {
      filetype = ext;
    }
  }
  lcl_define_take(interp, "filetype", lcl_string_new(filetype));

  // Get indent settings
  int tab_size = 4;
  bool use_tabs = true;
  if (host->current_editor) {
    // Use editor's settings
    // Note: We'd need to expose these, for now use defaults
  }
  lcl_define_take(interp, "tab_size", lcl_int_new(tab_size));
  lcl_define_take(interp, "use_tabs", lcl_int_new(use_tabs ? 1 : 0));

  // Cursor position
  size_t cursor = start;
  if (host->current_editor) {
    cursor = editor_get_cursor_pos(host->current_editor);
  }
  lcl_define_take(interp, "cursor", lcl_int_new((long)cursor));

  // Input from prompt (if provided as arg)
  const char *input = "";
  if (args && args[0]) {
    input = args[0];
  }
  lcl_define_take(interp, "input", lcl_string_new(input));

  // Evaluate the script
  lcl_value *result = NULL;
  int rc = lcl_eval_string(interp, data->script, &result);

  free(text);

  if (rc != LCL_RC_OK) {
    const char *err = lcl_interp_error_msg(interp);
    fprintf(stderr, "transform script error: %s\n",
            err ? err : "unknown error");
    if (result) {
      lcl_ref_dec(result);
    }
    return NULL;
  }

  // Parse the result
  char *prompt_msg = NULL;
  char *prompt_default = NULL;
  Patch *patch = parse_lcl_transform_result(result, data->name, start, end,
                                            &prompt_msg, &prompt_default);

  if (result) {
    lcl_ref_dec(result);
  }

  // Handle prompt request - store in host for UI to show
  if (prompt_msg) {
    lcl_host_set_transform_prompt(host, data->name, prompt_msg, prompt_default,
                                  buffer, start, end);
    free(prompt_msg);
    free(prompt_default);
    return NULL;
  }

  return patch;
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
  data->name = strdup(name);
  data->script = strdup(script);
  if (!data->name || !data->script) {
    lcl_transform_cleanup(data);
    return false;
  }

  if (!transform_register(name, description, lcl_transform_fn,
                          lcl_transform_cleanup, data)) {
    lcl_transform_cleanup(data);
    return false;
  }

  return true;
}
