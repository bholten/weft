#ifndef WEFT_BUFFER_H
#define WEFT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "annot_persist.h"
#include "annot_store.h"
#include "document.h"

// Buffer: wraps a Document with metadata for multi-buffer management
typedef struct Buffer {
  uint32_t id;        // Unique identifier
  Document *document; // Owned
  AnnotStore *annots; // Owned - annotation store with stable anchors
  char *file_path;    // Owned, NULL if untitled
  bool modified;      // Has unsaved changes
  int ref_count;      // Reference count for shared views

  // Persistent document ID (for annotation storage)
  char doc_id[DOC_ID_LEN]; // UUID string, empty if no persistence

  // Orphaned annotations (failed to re-anchor after document changes)
  PersistedAnnot *orphaned_annots;
  size_t orphaned_count;
  size_t orphaned_capacity;
} Buffer;

// BufferManager: manages all open buffers
typedef struct BufferManager {
  Buffer **buffers;
  size_t count;
  size_t capacity;
  uint32_t next_id;
} BufferManager;

// BufferManager lifecycle
BufferManager *buffer_manager_new(void);
void buffer_manager_free(BufferManager *bm);

// Buffer creation/destruction via manager
Buffer *buffer_manager_create(BufferManager *bm);
Buffer *buffer_manager_open_file(BufferManager *bm, const char *path);
void buffer_manager_close(BufferManager *bm, Buffer *buf);

// Find buffer by file path (returns NULL if not found)
Buffer *buffer_manager_find_by_path(BufferManager *bm, const char *path);

// Find buffer by ID (returns NULL if not found)
Buffer *buffer_manager_find_by_id(BufferManager *bm, uint32_t id);

// Buffer reference counting
void buffer_ref(Buffer *buf);
void buffer_unref(Buffer *buf);

// Buffer file operations
bool buffer_save(Buffer *buf);
bool buffer_save_as(Buffer *buf, const char *path);
bool buffer_load(Buffer *buf, const char *path);

// Buffer queries
const char *buffer_get_file_path(const Buffer *buf);
bool buffer_is_modified(const Buffer *buf);
void buffer_set_modified(Buffer *buf, bool modified);

// Get document revision (from annotation store, used for cache invalidation)
uint64_t buffer_get_doc_rev(const Buffer *buf);

// === Annotation Persistence ===

// Get the persistent document ID for a buffer (creates one if needed)
// reg: Document registry to use for ID lookup/creation
// Returns pointer to buffer's doc_id (do not free), or NULL on error
const char *buffer_get_doc_id(Buffer *buf, DocRegistry *reg);

// Save annotations to persistent storage
// Returns true on success
bool buffer_save_annotations(Buffer *buf, DocRegistry *reg,
                             const char *data_dir,
                             const char *const *persistent_layers,
                             size_t persistent_layer_count);

// Load annotations from persistent storage
// Returns number of annotations loaded (0 if none or error)
size_t buffer_load_annotations(Buffer *buf, DocRegistry *reg,
                               const char *data_dir);

// Get orphaned annotation count
size_t buffer_orphan_count(const Buffer *buf);

// Get orphaned annotation by index (returns NULL if out of range)
const PersistedAnnot *buffer_get_orphan(const Buffer *buf, size_t index);

// Resolve an orphaned annotation to a new position
// Returns annotation ID on success, 0 on failure
uint32_t buffer_resolve_orphan(Buffer *buf, size_t index, size_t start,
                               size_t end);

// Discard an orphaned annotation
void buffer_discard_orphan(Buffer *buf, size_t index);

// Clear all orphaned annotations
void buffer_clear_orphans(Buffer *buf);

#endif // WEFT_BUFFER_H
