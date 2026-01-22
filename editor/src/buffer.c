#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "annot_persist.h"

#define BUFFER_INITIAL_CAPACITY 8
#define ORPHAN_INITIAL_CAPACITY 8

// Internal: Create a buffer with given ID
static Buffer *buffer_new(uint32_t id) {
  Buffer *buf = calloc(1, sizeof(Buffer));
  if (!buf) {
    return NULL;
  }

  buf->id = id;
  buf->document = document_new();
  if (!buf->document) {
    free(buf);
    return NULL;
  }

  buf->annots = annot_store_new();
  if (!buf->annots) {
    document_free(buf->document);
    free(buf);
    return NULL;
  }

  buf->file_path = NULL;
  buf->modified = false;
  buf->ref_count = 1;

  // Initialize persistence fields
  buf->doc_id[0] = '\0';
  buf->orphaned_annots = NULL;
  buf->orphaned_count = 0;
  buf->orphaned_capacity = 0;

  return buf;
}

// Internal: Free a buffer (does not check ref count)
static void buffer_free(Buffer *buf) {
  if (!buf) {
    return;
  }
  document_free(buf->document);
  annot_store_free(buf->annots);
  free(buf->file_path);

  // Free orphaned annotations
  if (buf->orphaned_annots) {
    for (size_t i = 0; i < buf->orphaned_count; i++) {
      persisted_annot_free(&buf->orphaned_annots[i]);
    }
    free(buf->orphaned_annots);
  }

  free(buf);
}

BufferManager *buffer_manager_new(void) {
  BufferManager *bm = calloc(1, sizeof(BufferManager));
  if (!bm) {
    return NULL;
  }

  bm->buffers = calloc(BUFFER_INITIAL_CAPACITY, sizeof(Buffer *));
  if (!bm->buffers) {
    free(bm);
    return NULL;
  }

  bm->count = 0;
  bm->capacity = BUFFER_INITIAL_CAPACITY;
  bm->next_id = 1;

  return bm;
}

void buffer_manager_free(BufferManager *bm) {
  if (!bm) {
    return;
  }

  for (size_t i = 0; i < bm->count; i++) {
    buffer_free(bm->buffers[i]);
  }
  free(bm->buffers);
  free(bm);
}

Buffer *buffer_manager_create(BufferManager *bm) {
  if (!bm) {
    return NULL;
  }

  // Grow buffer array if needed
  if (bm->count >= bm->capacity) {
    size_t new_cap = bm->capacity * 2;
    Buffer **new_buffers = realloc(bm->buffers, new_cap * sizeof(Buffer *));
    if (!new_buffers) {
      return NULL;
    }
    bm->buffers = new_buffers;
    bm->capacity = new_cap;
  }

  Buffer *buf = buffer_new(bm->next_id++);
  if (!buf) {
    return NULL;
  }

  bm->buffers[bm->count++] = buf;
  return buf;
}

Buffer *buffer_manager_open_file(BufferManager *bm, const char *path) {
  if (!bm || !path) {
    return NULL;
  }

  Buffer *buf = buffer_manager_create(bm);
  if (!buf) {
    return NULL;
  }

  if (!buffer_load(buf, path)) {
    buffer_manager_close(bm, buf);
    return NULL;
  }

  return buf;
}

void buffer_manager_close(BufferManager *bm, Buffer *buf) {
  if (!bm || !buf) {
    return;
  }

  // Find buffer in array
  for (size_t i = 0; i < bm->count; i++) {
    if (bm->buffers[i] == buf) {
      // Decrement ref count
      buffer_unref(buf);

      // If ref count reached zero, remove from array
      if (buf->ref_count <= 0) {
        buffer_free(buf);
        // Shift remaining buffers
        for (size_t j = i; j < bm->count - 1; j++) {
          bm->buffers[j] = bm->buffers[j + 1];
        }
        bm->count--;
      }
      return;
    }
  }
}

Buffer *buffer_manager_find_by_path(BufferManager *bm, const char *path) {
  if (!bm || !path) {
    return NULL;
  }

  for (size_t i = 0; i < bm->count; i++) {
    Buffer *buf = bm->buffers[i];
    if (buf->file_path && strcmp(buf->file_path, path) == 0) {
      return buf;
    }
  }

  return NULL;
}

Buffer *buffer_manager_find_by_id(BufferManager *bm, uint32_t id) {
  if (!bm || id == 0) {
    return NULL;
  }

  for (size_t i = 0; i < bm->count; i++) {
    Buffer *buf = bm->buffers[i];
    if (buf->id == id) {
      return buf;
    }
  }

  return NULL;
}

void buffer_ref(Buffer *buf) {
  if (buf) {
    buf->ref_count++;
  }
}

void buffer_unref(Buffer *buf) {
  if (buf && buf->ref_count > 0) {
    buf->ref_count--;
  }
}

bool buffer_load(Buffer *buf, const char *path) {
  if (!buf || !path) {
    return false;
  }

  FILE *f = fopen(path, "rb");
  if (!f) {
    return false;
  }

  // Get file size
  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (file_size < 0) {
    fclose(f);
    return false;
  }

  // Read file contents
  char *content = malloc((size_t)file_size + 1);
  if (!content) {
    fclose(f);
    return false;
  }

  size_t bytes_read = fread(content, 1, (size_t)file_size, f);
  fclose(f);
  content[bytes_read] = '\0';

  // Clear existing document content and annotations
  size_t doc_len = document_length(buf->document);
  if (doc_len > 0) {
    document_delete(buf->document, 0, doc_len);
  }
  annot_store_clear(buf->annots);

  // Insert new content
  if (bytes_read > 0) {
    document_insert(buf->document, 0, content, bytes_read);
  }
  free(content);

  // Update file path
  free(buf->file_path);
  buf->file_path = strdup(path);
  buf->modified = false;

  return true;
}

bool buffer_save_as(Buffer *buf, const char *path) {
  if (!buf || !path) {
    return false;
  }

  FILE *f = fopen(path, "wb");
  if (!f) {
    return false;
  }

  size_t doc_len = document_length(buf->document);
  if (doc_len > 0) {
    // Write document content in chunks
    char chunk[4096];
    size_t offset = 0;
    while (offset < doc_len) {
      size_t to_write = doc_len - offset;
      if (to_write > sizeof(chunk)) {
        to_write = sizeof(chunk);
      }
      size_t got = document_get_text(buf->document, offset, chunk, to_write);
      if (fwrite(chunk, 1, got, f) != got) {
        fclose(f);
        return false;
      }
      offset += got;
    }
  }

  fclose(f);

  // Update file path (copy first to handle case where path == buf->file_path)
  char *new_path = strdup(path);
  if (!new_path) {
    return false;
  }
  free(buf->file_path);
  buf->file_path = new_path;
  buf->modified = false;

  return true;
}

bool buffer_save(Buffer *buf) {
  if (!buf || !buf->file_path) {
    return false;
  }
  return buffer_save_as(buf, buf->file_path);
}

const char *buffer_get_file_path(const Buffer *buf) {
  if (!buf) {
    return NULL;
  }
  return buf->file_path;
}

bool buffer_is_modified(const Buffer *buf) {
  if (!buf) {
    return false;
  }
  return buf->modified;
}

void buffer_set_modified(Buffer *buf, bool modified) {
  if (buf) {
    buf->modified = modified;
  }
}

uint64_t buffer_get_doc_rev(const Buffer *buf) {
  if (!buf || !buf->annots) {
    return 0;
  }
  return annot_store_get_doc_rev(buf->annots);
}

// === Annotation Persistence ===

const char *buffer_get_doc_id(Buffer *buf, DocRegistry *reg) {
  if (!buf || !reg) {
    return NULL;
  }

  // If buffer has no file path, we can't persist annotations
  if (!buf->file_path) {
    return NULL;
  }

  // If we already have a doc_id, return it
  if (buf->doc_id[0] != '\0') {
    return buf->doc_id;
  }

  // Get or create doc_id from registry
  const char *id = doc_registry_get_or_create(reg, buf->file_path);
  if (id) {
    strncpy(buf->doc_id, id, DOC_ID_LEN - 1);
    buf->doc_id[DOC_ID_LEN - 1] = '\0';
    return buf->doc_id;
  }

  return NULL;
}

bool buffer_save_annotations(Buffer *buf, DocRegistry *reg,
                             const char *data_dir,
                             const char *const *persistent_layers,
                             size_t persistent_layer_count) {
  if (!buf || !reg || !data_dir) {
    return false;
  }

  // Get doc_id (creates one if needed)
  const char *doc_id = buffer_get_doc_id(buf, reg);
  if (!doc_id) {
    return false;
  }

  // Update mtime in registry
  if (buf->file_path) {
    uint64_t mtime = get_file_mtime(buf->file_path);
    doc_registry_update_mtime(reg, buf->file_path, mtime);
  }

  // Save annotations
  return annot_persist_save(buf->annots, buf->document, doc_id, data_dir,
                            persistent_layers, persistent_layer_count);
}

size_t buffer_load_annotations(Buffer *buf, DocRegistry *reg,
                               const char *data_dir) {
  if (!buf || !reg || !data_dir || !buf->file_path) {
    return 0;
  }

  // Get doc_id from registry
  const char *doc_id = doc_registry_get(reg, buf->file_path);
  if (!doc_id) {
    // No annotations stored for this file
    return 0;
  }

  // Store doc_id in buffer
  strncpy(buf->doc_id, doc_id, DOC_ID_LEN - 1);
  buf->doc_id[DOC_ID_LEN - 1] = '\0';

  // Load persisted annotations
  size_t annot_count = 0;
  PersistedAnnot *persisted = annot_persist_load(doc_id, data_dir, &annot_count);
  if (!persisted || annot_count == 0) {
    return 0;
  }

  // Re-anchor annotations
  size_t result_count = 0;
  ReanchorResult *results =
      annot_persist_reanchor(persisted, annot_count, buf->document, &result_count);
  if (!results) {
    persisted_annot_array_free(persisted, annot_count);
    return 0;
  }

  // Restore successfully re-anchored annotations
  size_t restored = annot_persist_restore(buf->annots, results, result_count);

  // Collect orphaned annotations
  for (size_t i = 0; i < result_count; i++) {
    if (results[i].status == ANCHOR_ORPHANED) {
      // Grow orphan array if needed
      if (buf->orphaned_count >= buf->orphaned_capacity) {
        size_t new_cap = buf->orphaned_capacity == 0
                             ? ORPHAN_INITIAL_CAPACITY
                             : buf->orphaned_capacity * 2;
        PersistedAnnot *new_orphans =
            realloc(buf->orphaned_annots, new_cap * sizeof(PersistedAnnot));
        if (!new_orphans) {
          break;
        }
        buf->orphaned_annots = new_orphans;
        buf->orphaned_capacity = new_cap;
      }

      // Copy the orphaned annotation (move ownership)
      PersistedAnnot *pa = results[i].annot;
      buf->orphaned_annots[buf->orphaned_count] = *pa;
      // Clear the source so it doesn't get freed
      memset(pa, 0, sizeof(*pa));
      buf->orphaned_count++;
    }
  }

  // Free results (but not the annotations themselves, they're now owned by
  // the orphan list or the annotation store)
  free(results);

  // Free the persisted array (contents already moved)
  // Only free annotations that were successfully restored (not orphaned)
  for (size_t i = 0; i < annot_count; i++) {
    // Check if this annotation was orphaned (already moved)
    bool was_orphaned = false;
    for (size_t j = 0; j < buf->orphaned_count; j++) {
      if (buf->orphaned_annots[j].layer == persisted[i].layer) {
        // This one was moved to orphans, skip
        was_orphaned = true;
        break;
      }
    }
    if (!was_orphaned) {
      persisted_annot_free(&persisted[i]);
    }
  }
  free(persisted);

  return restored;
}

size_t buffer_orphan_count(const Buffer *buf) {
  return buf ? buf->orphaned_count : 0;
}

const PersistedAnnot *buffer_get_orphan(const Buffer *buf, size_t index) {
  if (!buf || index >= buf->orphaned_count) {
    return NULL;
  }
  return &buf->orphaned_annots[index];
}

uint32_t buffer_resolve_orphan(Buffer *buf, size_t index, size_t start,
                               size_t end) {
  if (!buf || index >= buf->orphaned_count) {
    return 0;
  }

  PersistedAnnot *orphan = &buf->orphaned_annots[index];

  // Add annotation to store
  uint32_t id = annot_store_add(buf->annots, start, end, orphan->layer,
                                (const char **)orphan->keys,
                                (const char **)orphan->values,
                                orphan->meta_count);

  if (id != 0) {
    // Successfully added, remove from orphan list
    buffer_discard_orphan(buf, index);
  }

  return id;
}

void buffer_discard_orphan(Buffer *buf, size_t index) {
  if (!buf || index >= buf->orphaned_count) {
    return;
  }

  // Free the orphan's data
  persisted_annot_free(&buf->orphaned_annots[index]);

  // Shift remaining orphans down
  for (size_t i = index; i < buf->orphaned_count - 1; i++) {
    buf->orphaned_annots[i] = buf->orphaned_annots[i + 1];
  }
  buf->orphaned_count--;
}

void buffer_clear_orphans(Buffer *buf) {
  if (!buf) {
    return;
  }

  for (size_t i = 0; i < buf->orphaned_count; i++) {
    persisted_annot_free(&buf->orphaned_annots[i]);
  }
  buf->orphaned_count = 0;
}
