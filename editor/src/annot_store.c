#include "annot_store.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_ANCHOR_CAP 64
#define INITIAL_RECORD_CAP 32
#define INITIAL_LAYER_CAP 8
#define INITIAL_QUERY_CAP 16

// === Internal Helpers ===

static char *strdup_safe(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *dup = malloc(len);
    if (dup) {
        memcpy(dup, s, len);
    }
    return dup;
}

// Find anchor by ID (returns NULL if not found)
static Anchor *find_anchor(AnnotStore *s, uint32_t id) {
    for (size_t i = 0; i < s->anchor_count; i++) {
        if (s->anchors[i].id == id) {
            return &s->anchors[i];
        }
    }
    return NULL;
}

static const Anchor *find_anchor_const(const AnnotStore *s, uint32_t id) {
    for (size_t i = 0; i < s->anchor_count; i++) {
        if (s->anchors[i].id == id) {
            return &s->anchors[i];
        }
    }
    return NULL;
}

// Find record by ID (returns NULL if not found)
static AnnotRecord *find_record(AnnotStore *s, uint32_t id) {
    for (size_t i = 0; i < s->record_count; i++) {
        if (s->records[i].id == id) {
            return &s->records[i];
        }
    }
    return NULL;
}

static const AnnotRecord *find_record_const(const AnnotStore *s, uint32_t id) {
    for (size_t i = 0; i < s->record_count; i++) {
        if (s->records[i].id == id) {
            return &s->records[i];
        }
    }
    return NULL;
}

// Find layer by name (returns NULL if not found)
static Layer *find_layer(AnnotStore *s, const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < s->layer_count; i++) {
        if (s->layers[i].name && strcmp(s->layers[i].name, name) == 0) {
            return &s->layers[i];
        }
    }
    return NULL;
}

static const Layer *find_layer_const(const AnnotStore *s, const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < s->layer_count; i++) {
        if (s->layers[i].name && strcmp(s->layers[i].name, name) == 0) {
            return &s->layers[i];
        }
    }
    return NULL;
}

// Create an anchor (returns 0 on failure)
static uint32_t create_anchor(AnnotStore *s, size_t pos, AnchorBias bias) {
    // Grow array if needed
    if (s->anchor_count >= s->anchor_cap) {
        size_t new_cap = s->anchor_cap * 2;
        Anchor *new_anchors = realloc(s->anchors, new_cap * sizeof(Anchor));
        if (!new_anchors) {
            return 0;
        }
        s->anchors = new_anchors;
        s->anchor_cap = new_cap;
    }

    uint32_t id = s->next_anchor_id++;
    Anchor *a = &s->anchors[s->anchor_count++];
    a->pos = pos;
    a->bias = bias;
    a->id = id;

    return id;
}

// Free record metadata
static void record_free_meta(AnnotRecord *r) {
    if (!r) {
        return;
    }
    for (size_t i = 0; i < r->meta_count; i++) {
        free(r->keys[i]);
        free(r->values[i]);
    }
    free(r->keys);
    free(r->values);
    free(r->layer);
    r->keys = NULL;
    r->values = NULL;
    r->layer = NULL;
    r->meta_count = 0;
}

// Remove anchor by ID (does not check if in use)
static void remove_anchor(AnnotStore *s, uint32_t id) {
    for (size_t i = 0; i < s->anchor_count; i++) {
        if (s->anchors[i].id == id) {
            // Move last element to this slot
            if (i < s->anchor_count - 1) {
                s->anchors[i] = s->anchors[s->anchor_count - 1];
            }
            s->anchor_count--;
            return;
        }
    }
}

// Add ID to query result
static bool query_add(AnnotQuery *q, uint32_t id) {
    if (q->count >= q->capacity) {
        size_t new_cap = q->capacity == 0 ? INITIAL_QUERY_CAP : q->capacity * 2;
        uint32_t *new_ids = realloc(q->ids, new_cap * sizeof(uint32_t));
        if (!new_ids) {
            return false;
        }
        q->ids = new_ids;
        q->capacity = new_cap;
    }
    q->ids[q->count++] = id;
    return true;
}

// === Lifecycle ===

AnnotStore *annot_store_new(void) {
    AnnotStore *s = calloc(1, sizeof(AnnotStore));
    if (!s) {
        return NULL;
    }

    s->anchors = malloc(INITIAL_ANCHOR_CAP * sizeof(Anchor));
    if (!s->anchors) {
        free(s);
        return NULL;
    }
    s->anchor_cap = INITIAL_ANCHOR_CAP;
    s->anchor_count = 0;
    s->next_anchor_id = 1;  // Reserve 0 as invalid

    s->records = malloc(INITIAL_RECORD_CAP * sizeof(AnnotRecord));
    if (!s->records) {
        free(s->anchors);
        free(s);
        return NULL;
    }
    s->record_cap = INITIAL_RECORD_CAP;
    s->record_count = 0;
    s->next_record_id = 1;  // Reserve 0 as invalid

    s->layers = malloc(INITIAL_LAYER_CAP * sizeof(Layer));
    if (!s->layers) {
        free(s->records);
        free(s->anchors);
        free(s);
        return NULL;
    }
    s->layer_cap = INITIAL_LAYER_CAP;
    s->layer_count = 0;

    s->doc_rev = 0;

    return s;
}

void annot_store_free(AnnotStore *s) {
    if (!s) {
        return;
    }

    // Free all records
    for (size_t i = 0; i < s->record_count; i++) {
        record_free_meta(&s->records[i]);
    }
    free(s->records);

    // Free all layers
    for (size_t i = 0; i < s->layer_count; i++) {
        free(s->layers[i].name);
    }
    free(s->layers);

    free(s->anchors);
    free(s);
}

void annot_store_clear(AnnotStore *s) {
    if (!s) {
        return;
    }

    // Free record metadata
    for (size_t i = 0; i < s->record_count; i++) {
        record_free_meta(&s->records[i]);
    }
    s->record_count = 0;
    s->anchor_count = 0;

    // Mark all layers dirty
    for (size_t i = 0; i < s->layer_count; i++) {
        s->layers[i].state = LAYER_DIRTY;
        s->layers[i].version++;
    }
}

void annot_store_clear_layer(AnnotStore *s, const char *layer) {
    if (!s || !layer) {
        return;
    }

    // Remove all records in this layer
    size_t write_idx = 0;
    for (size_t i = 0; i < s->record_count; i++) {
        if (s->records[i].layer && strcmp(s->records[i].layer, layer) == 0) {
            // Remove anchors for this record
            remove_anchor(s, s->records[i].start_anchor);
            remove_anchor(s, s->records[i].end_anchor);
            record_free_meta(&s->records[i]);
        } else {
            // Keep this record
            if (write_idx != i) {
                s->records[write_idx] = s->records[i];
            }
            write_idx++;
        }
    }
    s->record_count = write_idx;

    // Mark layer dirty
    Layer *l = find_layer(s, layer);
    if (l) {
        l->state = LAYER_DIRTY;
        l->version++;
    }
}

// === Annotation CRUD ===

uint32_t annot_store_add(AnnotStore *s, size_t start, size_t end,
                         const char *layer, const char **keys,
                         const char **values, size_t meta_count) {
    if (!s || start >= end) {
        return 0;
    }

    // Grow records array if needed
    if (s->record_count >= s->record_cap) {
        size_t new_cap = s->record_cap * 2;
        AnnotRecord *new_records = realloc(s->records, new_cap * sizeof(AnnotRecord));
        if (!new_records) {
            return 0;
        }
        s->records = new_records;
        s->record_cap = new_cap;
    }

    // Create anchors
    // Start anchor: LEFT bias (stays put when text inserted at start)
    uint32_t start_anchor = create_anchor(s, start, ANCHOR_LEFT);
    if (start_anchor == 0) {
        return 0;
    }

    // End anchor: RIGHT bias (moves when text inserted at end)
    uint32_t end_anchor = create_anchor(s, end, ANCHOR_RIGHT);
    if (end_anchor == 0) {
        remove_anchor(s, start_anchor);
        return 0;
    }

    // Allocate metadata arrays
    char **new_keys = NULL;
    char **new_values = NULL;
    if (meta_count > 0) {
        new_keys = calloc(meta_count, sizeof(char *));
        new_values = calloc(meta_count, sizeof(char *));
        if (!new_keys || !new_values) {
            free(new_keys);
            free(new_values);
            remove_anchor(s, start_anchor);
            remove_anchor(s, end_anchor);
            return 0;
        }

        // Copy metadata
        for (size_t i = 0; i < meta_count; i++) {
            if (keys && keys[i]) {
                new_keys[i] = strdup_safe(keys[i]);
                if (!new_keys[i]) {
                    goto cleanup_meta;
                }
            }
            if (values && values[i]) {
                new_values[i] = strdup_safe(values[i]);
                if (!new_values[i]) {
                    goto cleanup_meta;
                }
            }
        }
    }

    // Create record
    uint32_t id = s->next_record_id++;
    AnnotRecord *r = &s->records[s->record_count++];
    r->id = id;
    r->start_anchor = start_anchor;
    r->end_anchor = end_anchor;
    r->layer = strdup_safe(layer);
    r->keys = new_keys;
    r->values = new_values;
    r->meta_count = meta_count;
    r->doc_rev = s->doc_rev;

    // Auto-register layer if not exists
    if (layer && !find_layer(s, layer)) {
        annot_store_register_layer(s, layer);
    }

    return id;

cleanup_meta:
    for (size_t i = 0; i < meta_count; i++) {
        free(new_keys[i]);
        free(new_values[i]);
    }
    free(new_keys);
    free(new_values);
    remove_anchor(s, start_anchor);
    remove_anchor(s, end_anchor);
    return 0;
}

bool annot_store_remove(AnnotStore *s, uint32_t id) {
    if (!s || id == 0) {
        return false;
    }

    for (size_t i = 0; i < s->record_count; i++) {
        if (s->records[i].id == id) {
            // Remove anchors
            remove_anchor(s, s->records[i].start_anchor);
            remove_anchor(s, s->records[i].end_anchor);

            // Free metadata
            record_free_meta(&s->records[i]);

            // Move last record to this slot
            if (i < s->record_count - 1) {
                s->records[i] = s->records[s->record_count - 1];
            }
            s->record_count--;
            return true;
        }
    }

    return false;
}

const AnnotRecord *annot_store_get(const AnnotStore *s, uint32_t id) {
    if (!s || id == 0) {
        return NULL;
    }
    return find_record_const(s, id);
}

bool annot_store_get_span(const AnnotStore *s, uint32_t id,
                          size_t *start, size_t *end) {
    if (!s || id == 0 || !start || !end) {
        return false;
    }

    const AnnotRecord *r = find_record_const(s, id);
    if (!r) {
        return false;
    }

    const Anchor *start_a = find_anchor_const(s, r->start_anchor);
    const Anchor *end_a = find_anchor_const(s, r->end_anchor);
    if (!start_a || !end_a) {
        return false;
    }

    *start = start_a->pos;
    *end = end_a->pos;
    return true;
}

const char *annot_store_get_meta(const AnnotStore *s, uint32_t id,
                                 const char *key) {
    if (!s || id == 0 || !key) {
        return NULL;
    }

    const AnnotRecord *r = find_record_const(s, id);
    if (!r) {
        return NULL;
    }

    for (size_t i = 0; i < r->meta_count; i++) {
        if (r->keys[i] && strcmp(r->keys[i], key) == 0) {
            return r->values[i];
        }
    }

    return NULL;
}

// === Queries ===

void annot_query_init(AnnotQuery *q) {
    if (q) {
        q->ids = NULL;
        q->count = 0;
        q->capacity = 0;
    }
}

void annot_query_free(AnnotQuery *q) {
    if (q) {
        free(q->ids);
        q->ids = NULL;
        q->count = 0;
        q->capacity = 0;
    }
}

void annot_query_clear(AnnotQuery *q) {
    if (q) {
        q->count = 0;
    }
}

void annot_store_at(const AnnotStore *s, size_t pos,
                    const char *layer, AnnotQuery *out) {
    if (!s || !out) {
        return;
    }

    for (size_t i = 0; i < s->record_count; i++) {
        const AnnotRecord *r = &s->records[i];

        // Filter by layer if specified
        if (layer) {
            if (!r->layer || strcmp(r->layer, layer) != 0) {
                continue;
            }
        }

        // Get span positions
        const Anchor *start_a = find_anchor_const(s, r->start_anchor);
        const Anchor *end_a = find_anchor_const(s, r->end_anchor);
        if (!start_a || !end_a) {
            continue;
        }

        // Check if position is within span
        if (pos >= start_a->pos && pos < end_a->pos) {
            query_add(out, r->id);
        }
    }
}

void annot_store_in_range(const AnnotStore *s, size_t start, size_t end,
                          const char *layer, AnnotQuery *out) {
    if (!s || !out || start >= end) {
        return;
    }

    for (size_t i = 0; i < s->record_count; i++) {
        const AnnotRecord *r = &s->records[i];

        // Filter by layer if specified
        if (layer) {
            if (!r->layer || strcmp(r->layer, layer) != 0) {
                continue;
            }
        }

        // Get span positions
        const Anchor *start_a = find_anchor_const(s, r->start_anchor);
        const Anchor *end_a = find_anchor_const(s, r->end_anchor);
        if (!start_a || !end_a) {
            continue;
        }

        // Check for overlap: span overlaps [start, end) if span.start < end && span.end > start
        if (start_a->pos < end && end_a->pos > start) {
            query_add(out, r->id);
        }
    }
}

void annot_store_by_layer(const AnnotStore *s, const char *layer,
                          AnnotQuery *out) {
    if (!s || !layer || !out) {
        return;
    }

    for (size_t i = 0; i < s->record_count; i++) {
        const AnnotRecord *r = &s->records[i];
        if (r->layer && strcmp(r->layer, layer) == 0) {
            query_add(out, r->id);
        }
    }
}

// === Edit Hooks ===

void annot_store_on_insert(AnnotStore *s, size_t pos, size_t len) {
    if (!s || len == 0) {
        return;
    }

    // Transform all anchors
    for (size_t i = 0; i < s->anchor_count; i++) {
        Anchor *a = &s->anchors[i];
        if (a->pos > pos) {
            // Anchor is after insert point: shift forward
            a->pos += len;
        } else if (a->pos == pos) {
            // Anchor is at insert point: depends on bias
            if (a->bias == ANCHOR_RIGHT) {
                a->pos += len;
            }
            // ANCHOR_LEFT stays put
        }
        // Anchors before insert point: unchanged
    }

    // Mark all layers dirty
    for (size_t i = 0; i < s->layer_count; i++) {
        s->layers[i].state = LAYER_DIRTY;
    }

    s->doc_rev++;
}

void annot_store_on_delete(AnnotStore *s, size_t pos, size_t len) {
    if (!s || len == 0) {
        return;
    }

    size_t end = pos + len;

    // Transform all anchors
    for (size_t i = 0; i < s->anchor_count; i++) {
        Anchor *a = &s->anchors[i];
        if (a->pos >= end) {
            // Anchor is after deletion range: shift backward
            a->pos -= len;
        } else if (a->pos > pos) {
            // Anchor is within deletion range: collapse to deletion point
            a->pos = pos;
        }
        // Anchors at or before pos: unchanged
    }

    // Remove zero-length spans
    size_t write_idx = 0;
    for (size_t i = 0; i < s->record_count; i++) {
        AnnotRecord *r = &s->records[i];
        const Anchor *start_a = find_anchor_const(s, r->start_anchor);
        const Anchor *end_a = find_anchor_const(s, r->end_anchor);

        bool remove = false;
        if (!start_a || !end_a) {
            remove = true;
        } else if (start_a->pos >= end_a->pos) {
            // Zero-length or inverted span
            remove = true;
        }

        if (remove) {
            remove_anchor(s, r->start_anchor);
            remove_anchor(s, r->end_anchor);
            record_free_meta(r);
        } else {
            if (write_idx != i) {
                s->records[write_idx] = s->records[i];
            }
            write_idx++;
        }
    }
    s->record_count = write_idx;

    // Mark all layers dirty
    for (size_t i = 0; i < s->layer_count; i++) {
        s->layers[i].state = LAYER_DIRTY;
    }

    s->doc_rev++;
}

// === Layer Management ===

void annot_store_register_layer(AnnotStore *s, const char *name) {
    if (!s || !name) {
        return;
    }

    // Check if already exists
    if (find_layer(s, name)) {
        return;
    }

    // Grow array if needed
    if (s->layer_count >= s->layer_cap) {
        size_t new_cap = s->layer_cap * 2;
        Layer *new_layers = realloc(s->layers, new_cap * sizeof(Layer));
        if (!new_layers) {
            return;
        }
        s->layers = new_layers;
        s->layer_cap = new_cap;
    }

    Layer *l = &s->layers[s->layer_count++];
    l->name = strdup_safe(name);
    l->state = LAYER_VALID;
    l->version = 1;
}

void annot_store_set_layer_dirty(AnnotStore *s, const char *name) {
    if (!s || !name) {
        return;
    }

    Layer *l = find_layer(s, name);
    if (l) {
        l->state = LAYER_DIRTY;
        l->version++;
    }
}

void annot_store_set_layer_valid(AnnotStore *s, const char *name) {
    if (!s || !name) {
        return;
    }

    Layer *l = find_layer(s, name);
    if (l) {
        l->state = LAYER_VALID;
    }
}

LayerState annot_store_layer_state(const AnnotStore *s, const char *name) {
    if (!s || !name) {
        return LAYER_DIRTY;
    }

    const Layer *l = find_layer_const(s, name);
    if (l) {
        return l->state;
    }
    return LAYER_DIRTY;
}

uint64_t annot_store_layer_version(const AnnotStore *s, const char *name) {
    if (!s || !name) {
        return 0;
    }

    const Layer *l = find_layer_const(s, name);
    if (l) {
        return l->version;
    }
    return 0;
}

// === Document Revision ===

void annot_store_set_doc_rev(AnnotStore *s, uint64_t rev) {
    if (s) {
        s->doc_rev = rev;
    }
}

uint64_t annot_store_get_doc_rev(const AnnotStore *s) {
    if (!s) {
        return 0;
    }
    return s->doc_rev;
}
