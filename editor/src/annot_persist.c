#include "annot_persist.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// === Utility Functions ===

char *get_xdg_data_dir(const char *app_name) {
  const char *xdg_data = getenv("XDG_DATA_HOME");
  char *base_dir = NULL;

  if (xdg_data && xdg_data[0] != '\0') {
    base_dir = strdup(xdg_data);
  } else {
    const char *home = getenv("HOME");
    if (!home) {
      return NULL;
    }
    size_t len = strlen(home) + strlen("/.local/share") + 1;
    base_dir = malloc(len);
    if (base_dir) {
      snprintf(base_dir, len, "%s/.local/share", home);
    }
  }

  if (!base_dir) {
    return NULL;
  }

  size_t len = strlen(base_dir) + 1 + strlen(app_name) + 1;
  char *result = malloc(len);
  if (result) {
    snprintf(result, len, "%s/%s", base_dir, app_name);
  }
  free(base_dir);
  return result;
}

bool mkdir_recursive(const char *path) {
  if (!path || path[0] == '\0') {
    return false;
  }

  char *tmp = strdup(path);
  if (!tmp) {
    return false;
  }

  size_t len = strlen(tmp);
  if (tmp[len - 1] == '/') {
    tmp[len - 1] = '\0';
  }

  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        free(tmp);
        return false;
      }
      *p = '/';
    }
  }

  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
    free(tmp);
    return false;
  }

  free(tmp);
  return true;
}

char *path_join(const char *dir, const char *filename) {
  if (!dir || !filename) {
    return NULL;
  }

  size_t dir_len = strlen(dir);
  size_t file_len = strlen(filename);
  bool needs_slash = (dir_len > 0 && dir[dir_len - 1] != '/');

  size_t len = dir_len + (needs_slash ? 1 : 0) + file_len + 1;
  char *result = malloc(len);
  if (!result) {
    return NULL;
  }

  if (needs_slash) {
    snprintf(result, len, "%s/%s", dir, filename);
  } else {
    snprintf(result, len, "%s%s", dir, filename);
  }
  return result;
}

char *path_canonical(const char *path) {
  if (!path) {
    return NULL;
  }
  char *resolved = realpath(path, NULL);
  return resolved;
}

uint64_t get_file_mtime(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return 0;
  }
  return (uint64_t)st.st_mtime;
}

// === UUID Generation ===

void doc_generate_uuid(char out[DOC_ID_LEN]) {
  unsigned char bytes[16];

  // Try /dev/urandom first
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    ssize_t n = read(fd, bytes, 16);
    close(fd);
    if (n != 16) {
      // Fall back to rand() if read failed
      for (int i = 0; i < 16; i++) {
        bytes[i] = (unsigned char)(rand() & 0xFF);
      }
    }
  } else {
    // Fall back to rand()
    for (int i = 0; i < 16; i++) {
      bytes[i] = (unsigned char)(rand() & 0xFF);
    }
  }

  // Set UUID version 4 and variant bits
  bytes[6] = (bytes[6] & 0x0F) | 0x40; // Version 4
  bytes[8] = (bytes[8] & 0x3F) | 0x80; // Variant 1

  // Format as UUID string
  snprintf(
      out, DOC_ID_LEN,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
      bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
      bytes[14], bytes[15]);
}

// === Context Anchor Operations ===

ContextAnchor context_anchor_create(const Document *doc, size_t pos,
                                    AnchorBias bias) {
  ContextAnchor ca = {0};
  ca.approx_pos = pos;
  ca.bias = bias;

  if (!doc) {
    return ca;
  }

  size_t doc_len = document_length(doc);

  // Get before context
  size_t before_start =
      (pos > CONTEXT_WINDOW_SIZE) ? pos - CONTEXT_WINDOW_SIZE : 0;
  size_t before_len = pos - before_start;
  if (before_len > 0) {
    ca.before_ctx = malloc(before_len + 1);
    if (ca.before_ctx) {
      ca.before_len =
          document_get_text(doc, before_start, ca.before_ctx, before_len);
      ca.before_ctx[ca.before_len] = '\0';
    }
  }

  // Get after context
  size_t after_len = (pos + CONTEXT_WINDOW_SIZE <= doc_len)
                         ? CONTEXT_WINDOW_SIZE
                         : doc_len - pos;
  if (after_len > 0) {
    ca.after_ctx = malloc(after_len + 1);
    if (ca.after_ctx) {
      ca.after_len = document_get_text(doc, pos, ca.after_ctx, after_len);
      ca.after_ctx[ca.after_len] = '\0';
    }
  }

  return ca;
}

void context_anchor_free(ContextAnchor *ca) {
  if (!ca) {
    return;
  }
  free(ca->before_ctx);
  free(ca->after_ctx);
  ca->before_ctx = NULL;
  ca->after_ctx = NULL;
  ca->before_len = 0;
  ca->after_len = 0;
}

// Helper: compute similarity score between two strings (0-100)
static int string_similarity(const char *a, size_t a_len, const char *b,
                             size_t b_len) {
  if (a_len == 0 && b_len == 0) {
    return 100;
  }
  if (a_len == 0 || b_len == 0) {
    return 0;
  }

  // Simple matching: count matching characters from the appropriate end
  size_t match_count = 0;
  size_t min_len = (a_len < b_len) ? a_len : b_len;

  for (size_t i = 0; i < min_len; i++) {
    if (a[i] == b[i]) {
      match_count++;
    } else {
      break; // Stop at first mismatch for prefix matching
    }
  }

  return (int)((match_count * 100) / min_len);
}

// Helper: compute context match score at a position (0-100)
static int context_match_score(const ContextAnchor *ca, const Document *doc,
                               size_t pos) {
  size_t doc_len = document_length(doc);
  int before_score = 0;
  int after_score = 0;

  // Check before context (compare from the end, matching backwards)
  if (ca->before_ctx && ca->before_len > 0 && pos > 0) {
    size_t before_start = (pos > ca->before_len) ? pos - ca->before_len : 0;
    size_t check_len = pos - before_start;

    char *buf = malloc(check_len + 1);
    if (buf) {
      size_t got = document_get_text(doc, before_start, buf, check_len);
      buf[got] = '\0';

      // Compare from the end (since we care about text immediately before pos)
      size_t ca_offset = (ca->before_len > got) ? ca->before_len - got : 0;
      const char *ca_cmp = ca->before_ctx + ca_offset;
      size_t ca_cmp_len = ca->before_len - ca_offset;

      before_score = string_similarity(ca_cmp, ca_cmp_len, buf, got);
      free(buf);
    }
  } else if (ca->before_len == 0 && pos == 0) {
    before_score = 100; // Both at start of document
  }

  // Check after context
  if (ca->after_ctx && ca->after_len > 0 && pos < doc_len) {
    size_t check_len =
        (pos + ca->after_len <= doc_len) ? ca->after_len : doc_len - pos;

    char *buf = malloc(check_len + 1);
    if (buf) {
      size_t got = document_get_text(doc, pos, buf, check_len);
      buf[got] = '\0';
      after_score = string_similarity(ca->after_ctx, ca->after_len, buf, got);
      free(buf);
    }
  } else if (ca->after_len == 0 && pos == doc_len) {
    after_score = 100; // Both at end of document
  }

  // Weight: 50% before, 50% after
  return (before_score + after_score) / 2;
}

size_t context_anchor_resolve(const ContextAnchor *ca, const Document *doc,
                              size_t max_search_radius, int *confidence_out) {
  if (!ca || !doc) {
    if (confidence_out) {
      *confidence_out = 0;
    }
    return SIZE_MAX;
  }

  size_t doc_len = document_length(doc);
  size_t best_pos = ca->approx_pos;
  int best_score = 0;

  // Clamp approx_pos to document bounds
  if (best_pos > doc_len) {
    best_pos = doc_len;
  }

  // Try exact position first
  best_score = context_match_score(ca, doc, best_pos);
  if (best_score == 100) {
    if (confidence_out) {
      *confidence_out = 100;
    }
    return best_pos;
  }

  // Expand search radius
  size_t search_radius = (max_search_radius > 0) ? max_search_radius : doc_len;
  size_t start =
      (ca->approx_pos > search_radius) ? ca->approx_pos - search_radius : 0;
  size_t end = (ca->approx_pos + search_radius < doc_len)
                   ? ca->approx_pos + search_radius
                   : doc_len;

  // Search outward from approx_pos
  for (size_t offset = 1; offset <= search_radius; offset++) {
    // Try position before
    if (ca->approx_pos >= offset && ca->approx_pos - offset >= start) {
      size_t try_pos = ca->approx_pos - offset;
      int score = context_match_score(ca, doc, try_pos);
      if (score > best_score) {
        best_score = score;
        best_pos = try_pos;
        if (score == 100) {
          break;
        }
      }
    }

    // Try position after
    if (ca->approx_pos + offset <= end) {
      size_t try_pos = ca->approx_pos + offset;
      int score = context_match_score(ca, doc, try_pos);
      if (score > best_score) {
        best_score = score;
        best_pos = try_pos;
        if (score == 100) {
          break;
        }
      }
    }
  }

  if (confidence_out) {
    *confidence_out = best_score;
  }

  return (best_score >= 30) ? best_pos : SIZE_MAX;
}

// === Persisted Annotation Operations ===

void persisted_annot_free(PersistedAnnot *pa) {
  if (!pa) {
    return;
  }
  context_anchor_free(&pa->start);
  context_anchor_free(&pa->end);
  free(pa->layer);
  for (size_t i = 0; i < pa->meta_count; i++) {
    free(pa->keys[i]);
    free(pa->values[i]);
  }
  free(pa->keys);
  free(pa->values);
  memset(pa, 0, sizeof(*pa));
}

void persisted_annot_array_free(PersistedAnnot *annots, size_t count) {
  if (!annots) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    persisted_annot_free(&annots[i]);
  }
  free(annots);
}

// === String Escaping for .weft Format ===

static char *escape_string(const char *s) {
  if (!s) {
    return strdup("");
  }
  size_t len = strlen(s);
  size_t escaped_len = len;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '"' || s[i] == '\\' || s[i] == '\n' || s[i] == '\r' ||
        s[i] == '\t') {
      escaped_len++;
    }
  }
  char *result = malloc(escaped_len + 1);
  if (!result) {
    return NULL;
  }
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '"' || s[i] == '\\') {
      result[j++] = '\\';
      result[j++] = s[i];
    } else if (s[i] == '\n') {
      result[j++] = '\\';
      result[j++] = 'n';
    } else if (s[i] == '\r') {
      result[j++] = '\\';
      result[j++] = 'r';
    } else if (s[i] == '\t') {
      result[j++] = '\\';
      result[j++] = 't';
    } else {
      result[j++] = s[i];
    }
  }
  result[j] = '\0';
  return result;
}

static size_t unescape_string(char *s, size_t len) {
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '\\' && i + 1 < len) {
      i++;
      switch (s[i]) {
      case 'n': s[j++] = '\n'; break;
      case 'r': s[j++] = '\r'; break;
      case 't': s[j++] = '\t'; break;
      default: s[j++] = s[i]; break;
      }
    } else {
      s[j++] = s[i];
    }
  }
  s[j] = '\0';
  return j;
}

// === Document Registry ===

DocRegistry *doc_registry_new(const char *data_dir) {
  DocRegistry *reg = calloc(1, sizeof(DocRegistry));
  if (!reg) {
    return NULL;
  }

  reg->data_dir = strdup(data_dir);
  if (!reg->data_dir) {
    free(reg);
    return NULL;
  }

  reg->capacity = 16;
  reg->entries = calloc(reg->capacity, sizeof(RegistryEntry));
  if (!reg->entries) {
    free(reg->data_dir);
    free(reg);
    return NULL;
  }

  return reg;
}

void doc_registry_free(DocRegistry *reg) {
  if (!reg) {
    return;
  }

  for (size_t i = 0; i < reg->count; i++) {
    free(reg->entries[i].file_path);
  }
  free(reg->entries);
  free(reg->data_dir);
  free(reg);
}

// Helper: find entry by path
static RegistryEntry *registry_find(DocRegistry *reg, const char *path) {
  for (size_t i = 0; i < reg->count; i++) {
    if (strcmp(reg->entries[i].file_path, path) == 0) {
      return &reg->entries[i];
    }
  }
  return NULL;
}

const char *doc_registry_get(const DocRegistry *reg, const char *path) {
  if (!reg || !path) {
    return NULL;
  }

  // Use canonical path for lookup
  char *canonical = path_canonical(path);
  const char *lookup_path = canonical ? canonical : path;

  for (size_t i = 0; i < reg->count; i++) {
    if (strcmp(reg->entries[i].file_path, lookup_path) == 0) {
      free(canonical);
      return reg->entries[i].id;
    }
  }

  free(canonical);
  return NULL;
}

const char *doc_registry_get_or_create(DocRegistry *reg, const char *path) {
  if (!reg || !path) {
    return NULL;
  }

  // Use canonical path
  char *canonical = path_canonical(path);
  const char *lookup_path = canonical ? canonical : path;

  RegistryEntry *entry = registry_find(reg, lookup_path);
  if (entry) {
    free(canonical);
    return entry->id;
  }

  // Create new entry
  if (reg->count >= reg->capacity) {
    size_t new_cap = reg->capacity * 2;
    RegistryEntry *new_entries =
        realloc(reg->entries, new_cap * sizeof(RegistryEntry));
    if (!new_entries) {
      free(canonical);
      return NULL;
    }
    reg->entries = new_entries;
    reg->capacity = new_cap;
  }

  entry = &reg->entries[reg->count];
  entry->file_path = canonical ? canonical : strdup(path);
  if (!entry->file_path) {
    return NULL;
  }

  doc_generate_uuid(entry->id);
  entry->last_modified = get_file_mtime(path);
  reg->count++;
  reg->dirty = true;

  return entry->id;
}

void doc_registry_update_mtime(DocRegistry *reg, const char *path,
                               uint64_t mtime) {
  if (!reg || !path) {
    return;
  }

  char *canonical = path_canonical(path);
  const char *lookup_path = canonical ? canonical : path;

  RegistryEntry *entry = registry_find(reg, lookup_path);
  if (entry && entry->last_modified != mtime) {
    entry->last_modified = mtime;
    reg->dirty = true;
  }

  free(canonical);
}

bool doc_registry_save(DocRegistry *reg) {
  if (!reg || !reg->data_dir) {
    return false;
  }

  char *registry_path = path_join(reg->data_dir, "registry.weft");
  if (!registry_path) {
    return false;
  }

  FILE *f = fopen(registry_path, "w");
  free(registry_path);
  if (!f) {
    return false;
  }

  fprintf(f, ";; Weft Document Registry\n\n");

  for (size_t i = 0; i < reg->count; i++) {
    RegistryEntry *entry = &reg->entries[i];
    char *escaped_path = escape_string(entry->file_path);
    if (!escaped_path) {
      fclose(f);
      return false;
    }

    fprintf(f, "{path \"%s\"\n", escaped_path);
    fprintf(f, "  id \"%s\"\n", entry->id);
    fprintf(f, "  mtime %lu}\n\n", (unsigned long)entry->last_modified);
    free(escaped_path);
  }

  fclose(f);
  reg->dirty = false;
  return true;
}

// Helper: parse a single block from text (registry or annotation)
// Returns position after block, or 0 on error
static size_t parse_block(const char *text, size_t text_len, size_t start_pos,
                          char ***out_keys, char ***out_values,
                          size_t *out_count) {
  *out_keys = NULL;
  *out_values = NULL;
  *out_count = 0;

  size_t pos = start_pos;

  // Skip to '{'
  while (pos < text_len && text[pos] != '{') {
    if (text[pos] == ';' && pos + 1 < text_len && text[pos + 1] == ';') {
      // Skip comment line
      while (pos < text_len && text[pos] != '\n') {
        pos++;
      }
    }
    pos++;
  }

  if (pos >= text_len) {
    return 0;
  }
  pos++; // Skip '{'

  // Temporary storage
  char *keys[128];
  char *values[128];
  size_t kv_count = 0;

  // Parse key-value pairs until '}'
  while (pos < text_len && text[pos] != '}') {
    // Skip whitespace
    while (pos < text_len && (text[pos] == ' ' || text[pos] == '\t' ||
                              text[pos] == '\n' || text[pos] == '\r')) {
      pos++;
    }

    if (pos >= text_len || text[pos] == '}') {
      break;
    }

    // Read key
    size_t key_start = pos;
    while (pos < text_len && text[pos] != ' ' && text[pos] != '\t' &&
           text[pos] != '\n' && text[pos] != '}' && text[pos] != '"') {
      pos++;
    }
    size_t key_len = pos - key_start;

    if (key_len == 0) {
      if (text[pos] == '}') {
        break;
      }
      pos++;
      continue;
    }

    char *key = malloc(key_len + 1);
    if (!key) {
      goto cleanup_error;
    }
    memcpy(key, text + key_start, key_len);
    key[key_len] = '\0';

    // Skip whitespace
    while (pos < text_len && (text[pos] == ' ' || text[pos] == '\t' ||
                              text[pos] == '\n' || text[pos] == '\r')) {
      pos++;
    }

    // Read value
    char *value = NULL;

    if (pos < text_len && text[pos] == '"') {
      // Quoted string
      pos++; // Skip opening quote
      size_t val_start = pos;

      while (pos < text_len) {
        if (text[pos] == '\\' && pos + 1 < text_len) {
          pos += 2;
        } else if (text[pos] == '"') {
          break;
        } else {
          pos++;
        }
      }

      size_t val_len = pos - val_start;
      value = malloc(val_len + 1);
      if (!value) {
        free(key);
        goto cleanup_error;
      }
      memcpy(value, text + val_start, val_len);
      value[val_len] = '\0';
      unescape_string(value, val_len);

      if (pos < text_len && text[pos] == '"') {
        pos++;
      }
    } else {
      // Plain token
      size_t val_start = pos;
      while (pos < text_len && text[pos] != ' ' && text[pos] != '\t' &&
             text[pos] != '\n' && text[pos] != '}') {
        pos++;
      }
      size_t val_len = pos - val_start;

      value = malloc(val_len + 1);
      if (!value) {
        free(key);
        goto cleanup_error;
      }
      memcpy(value, text + val_start, val_len);
      value[val_len] = '\0';
    }

    if (kv_count < 128) {
      keys[kv_count] = key;
      values[kv_count] = value;
      kv_count++;
    } else {
      free(key);
      free(value);
    }
  }

  // Skip closing '}'
  if (pos < text_len && text[pos] == '}') {
    pos++;
  }

  // Copy to output
  if (kv_count > 0) {
    *out_keys = malloc(kv_count * sizeof(char *));
    *out_values = malloc(kv_count * sizeof(char *));
    if (!*out_keys || !*out_values) {
      free(*out_keys);
      free(*out_values);
      *out_keys = NULL;
      *out_values = NULL;
      goto cleanup_error;
    }
    memcpy(*out_keys, keys, kv_count * sizeof(char *));
    memcpy(*out_values, values, kv_count * sizeof(char *));
    *out_count = kv_count;
  }

  return pos;

cleanup_error:
  for (size_t i = 0; i < kv_count; i++) {
    free(keys[i]);
    free(values[i]);
  }
  return 0;
}

// Helper: find value for key in parsed block
static const char *block_get(char **keys, char **values, size_t count,
                             const char *key) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(keys[i], key) == 0) {
      return values[i];
    }
  }
  return NULL;
}

bool doc_registry_load(DocRegistry *reg) {
  if (!reg || !reg->data_dir) {
    return false;
  }

  char *registry_path = path_join(reg->data_dir, "registry.weft");
  if (!registry_path) {
    return false;
  }

  FILE *f = fopen(registry_path, "r");
  free(registry_path);
  if (!f) {
    // No registry file is OK (first run)
    return true;
  }

  // Read entire file
  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (file_size <= 0) {
    fclose(f);
    return true;
  }

  char *content = malloc((size_t)file_size + 1);
  if (!content) {
    fclose(f);
    return false;
  }

  size_t bytes_read = fread(content, 1, (size_t)file_size, f);
  fclose(f);
  content[bytes_read] = '\0';

  // Parse blocks
  size_t pos = 0;
  while (pos < bytes_read) {
    char **keys = NULL;
    char **values = NULL;
    size_t count = 0;

    size_t next_pos =
        parse_block(content, bytes_read, pos, &keys, &values, &count);
    if (next_pos == 0) {
      break;
    }
    pos = next_pos;

    // Extract registry entry fields
    const char *path = block_get(keys, values, count, "path");
    const char *id = block_get(keys, values, count, "id");
    const char *mtime_str = block_get(keys, values, count, "mtime");

    if (path && id) {
      // Add entry
      if (reg->count >= reg->capacity) {
        size_t new_cap = reg->capacity * 2;
        RegistryEntry *new_entries =
            realloc(reg->entries, new_cap * sizeof(RegistryEntry));
        if (!new_entries) {
          for (size_t i = 0; i < count; i++) {
            free(keys[i]);
            free(values[i]);
          }
          free(keys);
          free(values);
          free(content);
          return false;
        }
        reg->entries = new_entries;
        reg->capacity = new_cap;
      }

      RegistryEntry *entry = &reg->entries[reg->count];
      entry->file_path = strdup(path);
      if (strlen(id) < DOC_ID_LEN) {
        strncpy(entry->id, id, DOC_ID_LEN - 1);
        entry->id[DOC_ID_LEN - 1] = '\0';
      }
      entry->last_modified =
          mtime_str ? (uint64_t)strtoull(mtime_str, NULL, 10) : 0;
      reg->count++;
    }

    // Free block data
    for (size_t i = 0; i < count; i++) {
      free(keys[i]);
      free(values[i]);
    }
    free(keys);
    free(values);
  }

  free(content);
  reg->dirty = false;
  return true;
}

// === Annotation Serialization ===

bool annot_persist_save(const AnnotStore *store, const Document *doc,
                        const char *doc_id, const char *data_dir,
                        const char *const *persistent_layers,
                        size_t persistent_layer_count) {
  if (!store || !doc || !doc_id || !data_dir) {
    return false;
  }

  // Build annots directory path
  char *annots_dir = path_join(data_dir, "annots");
  if (!annots_dir) {
    return false;
  }

  mkdir_recursive(annots_dir);

  // Build file path
  char filename[DOC_ID_LEN + 6]; // id + ".weft" + null
  snprintf(filename, sizeof(filename), "%s.weft", doc_id);
  char *file_path = path_join(annots_dir, filename);
  free(annots_dir);
  if (!file_path) {
    return false;
  }

  FILE *f = fopen(file_path, "w");
  free(file_path);
  if (!f) {
    return false;
  }

  fprintf(f, ";; Annotations for document %s\n\n", doc_id);

  // Helper to check if layer is persistent
  auto bool is_persistent(const char *layer) {
    // If no explicit list, use default behavior
    if (!persistent_layers || persistent_layer_count == 0) {
      // ts/plumb are ephemeral (regenerated from content)
      // "style" is persistent for user annotations
      if (strcmp(layer, "ts") == 0 || strcmp(layer, "plumb") == 0) {
        return false;
      }
      return true;
    }
    for (size_t i = 0; i < persistent_layer_count; i++) {
      if (strcmp(persistent_layers[i], layer) == 0) {
        return true;
      }
    }
    return false;
  }

  // Iterate through all annotations
  for (size_t i = 0; i < store->record_count; i++) {
    const AnnotRecord *rec = &store->records[i];
    if (!rec->layer || !is_persistent(rec->layer)) {
      continue;
    }

    // Get span positions
    size_t start_pos = 0, end_pos = 0;
    if (!annot_store_get_span(store, rec->id, &start_pos, &end_pos)) {
      continue;
    }

    // Create context anchors
    ContextAnchor start_ca = context_anchor_create(doc, start_pos, ANCHOR_LEFT);
    ContextAnchor end_ca = context_anchor_create(doc, end_pos, ANCHOR_RIGHT);

    // Write annotation block
    char *escaped_layer = escape_string(rec->layer);
    if (!escaped_layer) {
      context_anchor_free(&start_ca);
      context_anchor_free(&end_ca);
      fclose(f);
      return false;
    }

    fprintf(f, "{layer \"%s\"\n", escaped_layer);
    free(escaped_layer);

    // Start anchor
    fprintf(f, "  start-approx %zu\n", start_ca.approx_pos);
    if (start_ca.before_ctx && start_ca.before_len > 0) {
      char *escaped = escape_string(start_ca.before_ctx);
      if (escaped) {
        fprintf(f, "  start-before \"%s\"\n", escaped);
        free(escaped);
      }
    }
    if (start_ca.after_ctx && start_ca.after_len > 0) {
      char *escaped = escape_string(start_ca.after_ctx);
      if (escaped) {
        fprintf(f, "  start-after \"%s\"\n", escaped);
        free(escaped);
      }
    }
    fprintf(f, "  start-bias %s\n",
            start_ca.bias == ANCHOR_LEFT ? "left" : "right");

    // End anchor
    fprintf(f, "  end-approx %zu\n", end_ca.approx_pos);
    if (end_ca.before_ctx && end_ca.before_len > 0) {
      char *escaped = escape_string(end_ca.before_ctx);
      if (escaped) {
        fprintf(f, "  end-before \"%s\"\n", escaped);
        free(escaped);
      }
    }
    if (end_ca.after_ctx && end_ca.after_len > 0) {
      char *escaped = escape_string(end_ca.after_ctx);
      if (escaped) {
        fprintf(f, "  end-after \"%s\"\n", escaped);
        free(escaped);
      }
    }
    fprintf(f, "  end-bias %s\n",
            end_ca.bias == ANCHOR_LEFT ? "left" : "right");

    // Metadata (skip prov:* keys)
    for (size_t j = 0; j < rec->meta_count; j++) {
      if (strncmp(rec->keys[j], "prov:", 5) == 0) {
        continue;
      }
      char *escaped_val = escape_string(rec->values[j]);
      if (escaped_val) {
        fprintf(f, "  %s \"%s\"\n", rec->keys[j], escaped_val);
        free(escaped_val);
      }
    }

    fprintf(f, "}\n\n");

    context_anchor_free(&start_ca);
    context_anchor_free(&end_ca);
  }

  fclose(f);
  return true;
}

PersistedAnnot *annot_persist_load(const char *doc_id, const char *data_dir,
                                   size_t *out_count) {
  *out_count = 0;

  if (!doc_id || !data_dir) {
    return NULL;
  }

  // Build file path
  char *annots_dir = path_join(data_dir, "annots");
  if (!annots_dir) {
    return NULL;
  }

  char filename[DOC_ID_LEN + 6];
  snprintf(filename, sizeof(filename), "%s.weft", doc_id);
  char *file_path = path_join(annots_dir, filename);
  free(annots_dir);
  if (!file_path) {
    return NULL;
  }

  FILE *f = fopen(file_path, "r");
  free(file_path);
  if (!f) {
    return NULL; // No annotations file is OK
  }

  // Read entire file
  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (file_size <= 0) {
    fclose(f);
    return NULL;
  }

  char *content = malloc((size_t)file_size + 1);
  if (!content) {
    fclose(f);
    return NULL;
  }

  size_t bytes_read = fread(content, 1, (size_t)file_size, f);
  fclose(f);
  content[bytes_read] = '\0';

  // Parse annotations
  size_t annot_cap = 16;
  PersistedAnnot *annots = calloc(annot_cap, sizeof(PersistedAnnot));
  if (!annots) {
    free(content);
    return NULL;
  }

  size_t pos = 0;
  while (pos < bytes_read) {
    char **keys = NULL;
    char **values = NULL;
    size_t count = 0;

    size_t next_pos =
        parse_block(content, bytes_read, pos, &keys, &values, &count);
    if (next_pos == 0) {
      break;
    }
    pos = next_pos;

    // Extract annotation fields
    const char *layer = block_get(keys, values, count, "layer");
    if (!layer) {
      // Not a valid annotation block
      for (size_t i = 0; i < count; i++) {
        free(keys[i]);
        free(values[i]);
      }
      free(keys);
      free(values);
      continue;
    }

    // Grow array if needed
    if (*out_count >= annot_cap) {
      annot_cap *= 2;
      PersistedAnnot *new_annots =
          realloc(annots, annot_cap * sizeof(PersistedAnnot));
      if (!new_annots) {
        for (size_t i = 0; i < count; i++) {
          free(keys[i]);
          free(values[i]);
        }
        free(keys);
        free(values);
        persisted_annot_array_free(annots, *out_count);
        free(content);
        return NULL;
      }
      annots = new_annots;
    }

    PersistedAnnot *pa = &annots[*out_count];
    memset(pa, 0, sizeof(*pa));

    pa->layer = strdup(layer);

    // Parse context anchors
    const char *start_approx = block_get(keys, values, count, "start-approx");
    const char *start_before = block_get(keys, values, count, "start-before");
    const char *start_after = block_get(keys, values, count, "start-after");
    const char *start_bias = block_get(keys, values, count, "start-bias");

    pa->start.approx_pos =
        start_approx ? (size_t)strtoull(start_approx, NULL, 10) : 0;
    pa->start.before_ctx = start_before ? strdup(start_before) : NULL;
    pa->start.before_len =
        pa->start.before_ctx ? strlen(pa->start.before_ctx) : 0;
    pa->start.after_ctx = start_after ? strdup(start_after) : NULL;
    pa->start.after_len = pa->start.after_ctx ? strlen(pa->start.after_ctx) : 0;
    pa->start.bias = (start_bias && strcmp(start_bias, "right") == 0)
                         ? ANCHOR_RIGHT
                         : ANCHOR_LEFT;

    const char *end_approx = block_get(keys, values, count, "end-approx");
    const char *end_before = block_get(keys, values, count, "end-before");
    const char *end_after = block_get(keys, values, count, "end-after");
    const char *end_bias = block_get(keys, values, count, "end-bias");

    pa->end.approx_pos =
        end_approx ? (size_t)strtoull(end_approx, NULL, 10) : 0;
    pa->end.before_ctx = end_before ? strdup(end_before) : NULL;
    pa->end.before_len = pa->end.before_ctx ? strlen(pa->end.before_ctx) : 0;
    pa->end.after_ctx = end_after ? strdup(end_after) : NULL;
    pa->end.after_len = pa->end.after_ctx ? strlen(pa->end.after_ctx) : 0;
    pa->end.bias = (end_bias && strcmp(end_bias, "left") == 0) ? ANCHOR_LEFT
                                                               : ANCHOR_RIGHT;

    // Collect metadata (non-system keys)
    size_t meta_count = 0;
    for (size_t i = 0; i < count; i++) {
      // Skip system keys
      if (strcmp(keys[i], "layer") == 0 || strncmp(keys[i], "start-", 6) == 0 ||
          strncmp(keys[i], "end-", 4) == 0) {
        continue;
      }
      meta_count++;
    }

    if (meta_count > 0) {
      pa->keys = malloc(meta_count * sizeof(char *));
      pa->values = malloc(meta_count * sizeof(char *));
      if (pa->keys && pa->values) {
        size_t j = 0;
        for (size_t i = 0; i < count; i++) {
          if (strcmp(keys[i], "layer") == 0 ||
              strncmp(keys[i], "start-", 6) == 0 ||
              strncmp(keys[i], "end-", 4) == 0) {
            continue;
          }
          pa->keys[j] = strdup(keys[i]);
          pa->values[j] = strdup(values[i]);
          j++;
        }
        pa->meta_count = meta_count;
      }
    }

    (*out_count)++;

    // Free block data
    for (size_t i = 0; i < count; i++) {
      free(keys[i]);
      free(values[i]);
    }
    free(keys);
    free(values);
  }

  free(content);
  return annots;
}

// === Re-anchoring ===

ReanchorResult *annot_persist_reanchor(const PersistedAnnot *annots,
                                       size_t count, const Document *doc,
                                       size_t *out_count) {
  *out_count = 0;

  if (!annots || count == 0 || !doc) {
    return NULL;
  }

  ReanchorResult *results = calloc(count, sizeof(ReanchorResult));
  if (!results) {
    return NULL;
  }

  for (size_t i = 0; i < count; i++) {
    const PersistedAnnot *pa = &annots[i];
    ReanchorResult *res = &results[i];

    res->annot = (PersistedAnnot *)pa; // Cast away const for storage

    // Resolve start position
    int start_confidence = 0;
    res->new_start =
        context_anchor_resolve(&pa->start, doc, 1000, &start_confidence);

    // Resolve end position
    int end_confidence = 0;
    res->new_end = context_anchor_resolve(&pa->end, doc, 1000, &end_confidence);

    // Compute overall confidence and status
    res->confidence = (start_confidence + end_confidence) / 2;

    if (res->new_start == SIZE_MAX || res->new_end == SIZE_MAX) {
      res->status = ANCHOR_ORPHANED;
      res->new_start = pa->start.approx_pos;
      res->new_end = pa->end.approx_pos;
      res->confidence = 0;
    } else if (res->confidence >= 90) {
      res->status = ANCHOR_OK;
    } else if (res->confidence >= 60) {
      res->status = ANCHOR_FUZZY;
    } else {
      res->status = ANCHOR_ORPHANED;
    }

    // Ensure start <= end
    if (res->new_start > res->new_end) {
      size_t tmp = res->new_start;
      res->new_start = res->new_end;
      res->new_end = tmp;
    }
  }

  *out_count = count;
  return results;
}

size_t annot_persist_restore(AnnotStore *store, const ReanchorResult *results,
                             size_t count) {
  if (!store || !results) {
    return 0;
  }

  size_t restored = 0;

  for (size_t i = 0; i < count; i++) {
    const ReanchorResult *res = &results[i];

    // Skip orphaned annotations
    if (res->status == ANCHOR_ORPHANED) {
      continue;
    }

    const PersistedAnnot *pa = res->annot;

    // Add annotation to store
    uint32_t id = annot_store_add(store, res->new_start, res->new_end,
                                  pa->layer, (const char **)pa->keys,
                                  (const char **)pa->values, pa->meta_count);
    if (id != 0) {
      restored++;
    }
  }

  return restored;
}
