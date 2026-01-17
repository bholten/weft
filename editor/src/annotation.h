#ifndef WEFT_ANNOTATION_H
#define WEFT_ANNOTATION_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    size_t start;       // Byte offset
    size_t end;         // Byte offset (exclusive)
    char *layer;        // Layer name (e.g., "style", "lint")
    char *key;          // Metadata key
    char *value;        // Metadata value (JSON string for complex data)
} Span;

typedef struct Annotation {
    Span *spans;
    size_t spans_len;
    size_t spans_cap;
} Annotation;

// Lifecycle
Annotation *annotation_new(void);
void annotation_free(Annotation *a);

// Basic operations
bool annotation_add_span(Annotation *a, size_t start, size_t end,
                         const char *layer, const char *key, const char *value);
void annotation_clear(Annotation *a);

// Query (simple linear scan for now)
// Returns number of spans found, fills out array up to out_cap
size_t annotation_spans_at(const Annotation *a, size_t pos, Span **out, size_t out_cap);
size_t annotation_spans_in_range(const Annotation *a, size_t start, size_t end,
                                  Span **out, size_t out_cap);

#endif // WEFT_ANNOTATION_H
