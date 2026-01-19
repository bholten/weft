#include "patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "annot_store.h"
#include "buffer.h"
#include "document.h"

#define PATCH_INITIAL_CAPACITY 8

// === PatchEdit Implementation ===

PatchEdit *patch_edit_new(size_t start, size_t end, const char *insert) {
  PatchEdit *edit = calloc(1, sizeof(PatchEdit));
  if (!edit) {
    return NULL;
  }

  edit->start = start;
  edit->end = end;

  if (insert && insert[0] != '\0') {
    edit->insert_len = strlen(insert);
    edit->insert = malloc(edit->insert_len + 1);
    if (!edit->insert) {
      free(edit);
      return NULL;
    }
    memcpy(edit->insert, insert, edit->insert_len + 1);
  }

  return edit;
}

PatchEdit *patch_edit_new_with_check(size_t start, size_t end,
                                     const char *insert, const char *expected) {
  PatchEdit *edit = patch_edit_new(start, end, insert);
  if (!edit) {
    return NULL;
  }

  if (expected) {
    edit->expected_text = strdup(expected);
    if (!edit->expected_text) {
      patch_edit_free(edit);
      return NULL;
    }
  }

  return edit;
}

void patch_edit_free(PatchEdit *edit) {
  if (!edit) {
    return;
  }
  free(edit->insert);
  free(edit->expected_text);
  free(edit);
}

// === Patch Implementation ===

Patch *patch_new(const char *transform_name) {
  Patch *patch = calloc(1, sizeof(Patch));
  if (!patch) {
    return NULL;
  }

  if (transform_name) {
    patch->transform_name = strdup(transform_name);
  }

  return patch;
}

void patch_free(Patch *patch) {
  if (!patch) {
    return;
  }

  for (size_t i = 0; i < patch->edit_count; i++) {
    patch_edit_free(patch->edits[i]);
  }
  free(patch->edits);
  free(patch->transform_name);
  free(patch->description);
  free(patch->origin);
  free(patch);
}

static bool patch_grow(Patch *patch) {
  size_t new_cap = patch->edit_capacity == 0 ? PATCH_INITIAL_CAPACITY
                                             : patch->edit_capacity * 2;

  PatchEdit **new_edits = realloc(patch->edits, new_cap * sizeof(PatchEdit *));
  if (!new_edits) {
    return false;
  }

  patch->edits = new_edits;
  patch->edit_capacity = new_cap;
  return true;
}

bool patch_add_edit(Patch *patch, PatchEdit *edit) {
  if (!patch || !edit) {
    return false;
  }

  if (patch->edit_count >= patch->edit_capacity) {
    if (!patch_grow(patch)) {
      return false;
    }
  }

  patch->edits[patch->edit_count++] = edit;
  return true;
}

bool patch_add(Patch *patch, size_t start, size_t end, const char *insert) {
  PatchEdit *edit = patch_edit_new(start, end, insert);
  if (!edit) {
    return false;
  }

  if (!patch_add_edit(patch, edit)) {
    patch_edit_free(edit);
    return false;
  }

  return true;
}

void patch_set_description(Patch *patch, const char *description) {
  if (!patch) {
    return;
  }
  free(patch->description);
  patch->description = description ? strdup(description) : NULL;
}

void patch_set_origin(Patch *patch, const char *origin) {
  if (!patch) {
    return;
  }
  free(patch->origin);
  patch->origin = origin ? strdup(origin) : NULL;
}

void patch_set_target(Patch *patch, uint64_t doc_rev, size_t doc_len) {
  if (!patch) {
    return;
  }
  patch->target_doc_rev = doc_rev;
  patch->target_doc_len = doc_len;
}

// Comparison function for sorting edits by start position
static int compare_edits(const void *a, const void *b) {
  const PatchEdit *ea = *(const PatchEdit **)a;
  const PatchEdit *eb = *(const PatchEdit **)b;

  if (ea->start < eb->start) {
    return -1;
  }
  if (ea->start > eb->start) {
    return 1;
  }
  // Same start: longer edit first (so we can detect overlaps)
  if (ea->end > eb->end) {
    return -1;
  }
  if (ea->end < eb->end) {
    return 1;
  }
  return 0;
}

void patch_sort_edits(Patch *patch) {
  if (!patch || patch->edit_count < 2) {
    return;
  }
  qsort(patch->edits, patch->edit_count, sizeof(PatchEdit *), compare_edits);
}

bool patch_is_valid(const Patch *patch) {
  if (!patch) {
    return false;
  }

  // Check edits are sorted and non-overlapping
  for (size_t i = 1; i < patch->edit_count; i++) {
    const PatchEdit *prev = patch->edits[i - 1];
    const PatchEdit *curr = patch->edits[i];

    // Previous edit must end before current starts
    if (prev->end > curr->start) {
      return false; // Overlapping
    }
  }

  return true;
}

// === Patch Validation ===

PatchResult patch_validate(const Patch *patch, const Document *doc) {
  if (!patch || !doc) {
    return PATCH_ERR_INVALID;
  }

  size_t doc_len = document_length(doc);

  // Check document revision if we have target info
  if (patch->target_doc_rev != 0) {
    // Note: We'd need to add revision tracking to Document, or pass it
    // separately For now, just check length as a basic sanity check
    if (patch->target_doc_len != 0 && patch->target_doc_len != doc_len) {
      // Document length changed - patch might be stale
      // This is a soft warning, not necessarily an error
    }
  }

  // Check each edit
  for (size_t i = 0; i < patch->edit_count; i++) {
    const PatchEdit *edit = patch->edits[i];

    // Check bounds
    if (edit->start > doc_len || edit->end > doc_len) {
      return PATCH_ERR_OUT_OF_BOUNDS;
    }
    if (edit->start > edit->end) {
      return PATCH_ERR_INVALID;
    }

    // Check expected text if present
    if (edit->expected_text) {
      size_t expected_len = strlen(edit->expected_text);
      size_t actual_len = edit->end - edit->start;

      if (expected_len != actual_len) {
        return PATCH_ERR_MISMATCH;
      }

      char *actual = malloc(actual_len + 1);
      if (!actual) {
        return PATCH_ERR_INVALID;
      }

      document_get_text(doc, edit->start, actual, actual_len);
      actual[actual_len] = '\0';

      int match = strcmp(actual, edit->expected_text);
      free(actual);

      if (match != 0) {
        return PATCH_ERR_MISMATCH;
      }
    }
  }

  // Check for overlaps
  if (!patch_is_valid(patch)) {
    return PATCH_ERR_INVALID;
  }

  return PATCH_OK;
}

// === Patch Application ===

PatchResult patch_apply(const Patch *patch, Buffer *buffer) {
  if (!patch || !buffer || !buffer->document) {
    return PATCH_ERR_INVALID;
  }

  // Validate first
  PatchResult result = patch_validate(patch, buffer->document);
  if (result != PATCH_OK) {
    return result;
  }

  Document *doc = buffer->document;
  AnnotStore *annots = buffer->annots;

  // Apply edits in reverse order to maintain correct offsets
  // (Earlier edits would shift positions of later edits otherwise)
  for (size_t i = patch->edit_count; i > 0; i--) {
    const PatchEdit *edit = patch->edits[i - 1];
    size_t del_len = edit->end - edit->start;

    // Delete existing text if any
    if (del_len > 0) {
      document_delete(doc, edit->start, del_len);
      if (annots) {
        annot_store_on_delete(annots, edit->start, del_len);
      }
    }

    // Insert new text if any
    if (edit->insert && edit->insert_len > 0) {
      document_insert(doc, edit->start, edit->insert, edit->insert_len);
      if (annots) {
        annot_store_on_insert(annots, edit->start, edit->insert_len);
      }
    }
  }

  // Mark buffer as modified
  buffer_set_modified(buffer, true);

  return PATCH_OK;
}

// === Diff Generation ===

// Helper to find line boundaries
static size_t find_line_start(const Document *doc, size_t pos) {
  if (pos == 0) {
    return 0;
  }

  char c;
  size_t p = pos;
  while (p > 0) {
    document_get_text(doc, p - 1, &c, 1);
    if (c == '\n') {
      return p;
    }
    p--;
  }
  return 0;
}

static size_t find_line_end(const Document *doc, size_t pos) {
  size_t doc_len = document_length(doc);
  if (pos >= doc_len) {
    return doc_len;
  }

  char c;
  size_t p = pos;
  while (p < doc_len) {
    document_get_text(doc, p, &c, 1);
    if (c == '\n') {
      return p + 1; // Include the newline
    }
    p++;
  }
  return doc_len;
}

// Count lines in a string
static size_t count_lines(const char *text, size_t len) {
  if (!text || len == 0) {
    return 0;
  }

  size_t count = 1; // At least one line if there's any text
  for (size_t i = 0; i < len; i++) {
    if (text[i] == '\n') {
      count++;
    }
  }
  // Don't count trailing newline as extra line
  if (len > 0 && text[len - 1] == '\n') {
    count--;
  }
  return count;
}

char *patch_to_diff(const Patch *patch, const Document *doc,
                    int context_lines) {
  if (!patch || !doc) {
    return NULL;
  }

  // Simple implementation: generate a basic unified diff
  // For a full implementation, we'd want proper hunk merging

  size_t buf_size = 4096;
  size_t buf_used = 0;
  char *buf = malloc(buf_size);
  if (!buf) {
    return NULL;
  }
  buf[0] = '\0';

// Helper macro to append to buffer
#define APPEND(fmt, ...)                                                       \
  do {                                                                         \
    int needed =                                                               \
        snprintf(buf + buf_used, buf_size - buf_used, fmt, ##__VA_ARGS__);     \
    if (needed < 0) {                                                          \
      free(buf);                                                               \
      return NULL;                                                             \
    }                                                                          \
    if ((size_t)needed >= buf_size - buf_used) {                               \
      buf_size = buf_size * 2 + needed;                                        \
      char *new_buf = realloc(buf, buf_size);                                  \
      if (!new_buf) {                                                          \
        free(buf);                                                             \
        return NULL;                                                           \
      }                                                                        \
      buf = new_buf;                                                           \
      snprintf(buf + buf_used, buf_size - buf_used, fmt, ##__VA_ARGS__);       \
    }                                                                          \
    buf_used += needed;                                                        \
  } while (0)

  // Header
  APPEND("--- original\n");
  APPEND("+++ modified\n");

  (void)context_lines; // TODO: implement context lines

  for (size_t i = 0; i < patch->edit_count; i++) {
    const PatchEdit *edit = patch->edits[i];

    // Get the old text
    size_t old_len = edit->end - edit->start;
    char *old_text = NULL;
    if (old_len > 0) {
      old_text = malloc(old_len + 1);
      if (old_text) {
        document_get_text(doc, edit->start, old_text, old_len);
        old_text[old_len] = '\0';
      }
    }

    // Calculate line numbers (1-based for diff)
    size_t line_start = document_pos_to_line(doc, edit->start) + 1;
    size_t old_line_count = old_text ? count_lines(old_text, old_len) : 0;
    size_t new_line_count =
        edit->insert ? count_lines(edit->insert, edit->insert_len) : 0;

    if (old_line_count == 0) {
      old_line_count = 1;
    }
    if (new_line_count == 0 && edit->insert_len > 0) {
      new_line_count = 1;
    }

    // Hunk header
    APPEND("@@ -%zu,%zu +%zu,%zu @@\n", line_start, old_line_count, line_start,
           new_line_count);

    // Old lines (deletions)
    if (old_text) {
      char *line = old_text;
      char *end = old_text + old_len;
      while (line < end) {
        char *nl = memchr(line, '\n', end - line);
        size_t line_len = nl ? (size_t)(nl - line) : (size_t)(end - line);
        APPEND("-");
        // Write line content
        for (size_t j = 0; j < line_len; j++) {
          char c = line[j];
          if (c == '\0') {
            break;
          }
          buf[buf_used++] = c;
          if (buf_used >= buf_size - 1) {
            buf_size *= 2;
            char *new_buf = realloc(buf, buf_size);
            if (!new_buf) {
              free(buf);
              free(old_text);
              return NULL;
            }
            buf = new_buf;
          }
        }
        buf[buf_used] = '\0';
        APPEND("\n");
        line = nl ? nl + 1 : end;
      }
      free(old_text);
    }

    // New lines (additions)
    if (edit->insert && edit->insert_len > 0) {
      const char *line = edit->insert;
      const char *end = edit->insert + edit->insert_len;
      while (line < end) {
        const char *nl = memchr(line, '\n', end - line);
        size_t line_len = nl ? (size_t)(nl - line) : (size_t)(end - line);
        APPEND("+");
        for (size_t j = 0; j < line_len; j++) {
          char c = line[j];
          if (c == '\0') {
            break;
          }
          buf[buf_used++] = c;
          if (buf_used >= buf_size - 1) {
            buf_size *= 2;
            char *new_buf = realloc(buf, buf_size);
            if (!new_buf) {
              free(buf);
              return NULL;
            }
            buf = new_buf;
          }
        }
        buf[buf_used] = '\0';
        APPEND("\n");
        line = nl ? nl + 1 : end;
      }
    }
  }

#undef APPEND

  return buf;
}

void patch_stats(const Patch *patch, size_t *out_additions,
                 size_t *out_deletions, size_t *out_edit_count) {
  size_t additions = 0;
  size_t deletions = 0;

  if (patch) {
    for (size_t i = 0; i < patch->edit_count; i++) {
      const PatchEdit *edit = patch->edits[i];
      deletions += edit->end - edit->start;
      additions += edit->insert_len;
    }
  }

  if (out_additions) {
    *out_additions = additions;
  }
  if (out_deletions) {
    *out_deletions = deletions;
  }
  if (out_edit_count) {
    *out_edit_count = patch ? patch->edit_count : 0;
  }
}
