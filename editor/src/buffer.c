#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_INITIAL_CAPACITY 8

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

    // Update file path
    free(buf->file_path);
    buf->file_path = strdup(path);
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
