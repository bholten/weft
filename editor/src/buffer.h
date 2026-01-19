#ifndef WEFT_BUFFER_H
#define WEFT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "document.h"
#include "annot_store.h"

// Buffer: wraps a Document with metadata for multi-buffer management
typedef struct Buffer {
    uint32_t id;              // Unique identifier
    Document *document;       // Owned
    AnnotStore *annots;       // Owned - annotation store with stable anchors
    char *file_path;          // Owned, NULL if untitled
    bool modified;            // Has unsaved changes
    int ref_count;            // Reference count for shared views
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

#endif // WEFT_BUFFER_H
