#ifndef WEFT_ANNOT_STORE_H
#define WEFT_ANNOT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Anchor bias: how an anchor behaves when text is inserted at its position
typedef enum {
    ANCHOR_LEFT,   // Stays at position (text inserted after)
    ANCHOR_RIGHT   // Moves with insert (text inserted before)
} AnchorBias;

// Anchor: a position that survives document edits
typedef struct {
    size_t pos;           // Current byte offset
    AnchorBias bias;      // Behavior on insert at this position
    uint32_t id;          // Unique identifier
} Anchor;

// Annotation record: a span with metadata
typedef struct {
    uint32_t id;              // Unique identifier
    uint32_t start_anchor;    // Anchor ID for start
    uint32_t end_anchor;      // Anchor ID for end
    char *layer;              // Layer name (owned, e.g., "style", "lint")
    char **keys;              // Metadata keys "namespace:key" (owned)
    char **values;            // Metadata values (owned)
    size_t meta_count;        // Number of key/value pairs
    uint64_t doc_rev;         // Document revision when created
} AnnotRecord;

// Layer state
typedef enum {
    LAYER_VALID,
    LAYER_DIRTY
} LayerState;

// Layer: grouping and dirty tracking for annotations
typedef struct {
    char *name;           // Layer name (owned)
    LayerState state;     // Current state
    uint64_t version;     // Incremented on changes
} Layer;

// Query result: list of annotation IDs
typedef struct {
    uint32_t *ids;
    size_t count;
    size_t capacity;
} AnnotQuery;

// Annotation Store
typedef struct {
    Anchor *anchors;
    size_t anchor_count;
    size_t anchor_cap;
    uint32_t next_anchor_id;

    AnnotRecord *records;
    size_t record_count;
    size_t record_cap;
    uint32_t next_record_id;

    Layer *layers;
    size_t layer_count;
    size_t layer_cap;

    uint64_t doc_rev;   // Current document revision
} AnnotStore;

// === Lifecycle ===

AnnotStore *annot_store_new(void);
void annot_store_free(AnnotStore *s);
void annot_store_clear(AnnotStore *s);
void annot_store_clear_layer(AnnotStore *s, const char *layer);

// === Annotation CRUD ===

// Add an annotation spanning [start, end) with given metadata
// Returns annotation ID, or 0 on failure
uint32_t annot_store_add(AnnotStore *s, size_t start, size_t end,
                         const char *layer, const char **keys,
                         const char **values, size_t meta_count);

// Remove an annotation by ID
bool annot_store_remove(AnnotStore *s, uint32_t id);

// Get annotation record by ID (returns NULL if not found)
const AnnotRecord *annot_store_get(const AnnotStore *s, uint32_t id);

// Get the current span of an annotation (resolves anchors to positions)
bool annot_store_get_span(const AnnotStore *s, uint32_t id,
                          size_t *start, size_t *end);

// Get metadata value for a key from an annotation
const char *annot_store_get_meta(const AnnotStore *s, uint32_t id,
                                 const char *key);

// === Queries ===

// Initialize a query result (caller must free with annot_query_free)
void annot_query_init(AnnotQuery *q);
void annot_query_free(AnnotQuery *q);
void annot_query_clear(AnnotQuery *q);

// Find annotations containing position
void annot_store_at(const AnnotStore *s, size_t pos,
                    const char *layer, AnnotQuery *out);

// Find annotations overlapping range [start, end)
void annot_store_in_range(const AnnotStore *s, size_t start, size_t end,
                          const char *layer, AnnotQuery *out);

// Find all annotations in a layer
void annot_store_by_layer(const AnnotStore *s, const char *layer,
                          AnnotQuery *out);

// === Edit Hooks (called by editor on document changes) ===

void annot_store_on_insert(AnnotStore *s, size_t pos, size_t len);
void annot_store_on_delete(AnnotStore *s, size_t pos, size_t len);

// === Layer Management ===

void annot_store_register_layer(AnnotStore *s, const char *name);
void annot_store_set_layer_dirty(AnnotStore *s, const char *name);
LayerState annot_store_layer_state(const AnnotStore *s, const char *name);
uint64_t annot_store_layer_version(const AnnotStore *s, const char *name);

// === Document Revision ===

void annot_store_set_doc_rev(AnnotStore *s, uint64_t rev);
uint64_t annot_store_get_doc_rev(const AnnotStore *s);

#endif // WEFT_ANNOT_STORE_H
