#include "annotation.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_SPANS_CAP 32

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

static void span_free_contents(Span *span) {
    if (!span) {
        return;
    }
    free(span->layer);
    free(span->key);
    free(span->value);
    span->layer = NULL;
    span->key = NULL;
    span->value = NULL;
}

Annotation *annotation_new(void) {
    Annotation *a = calloc(1, sizeof(Annotation));
    if (!a) {
        return NULL;
    }

    a->spans = malloc(INITIAL_SPANS_CAP * sizeof(Span));
    if (!a->spans) {
        free(a);
        return NULL;
    }

    a->spans_cap = INITIAL_SPANS_CAP;
    a->spans_len = 0;

    return a;
}

void annotation_free(Annotation *a) {
    if (!a) {
        return;
    }

    for (size_t i = 0; i < a->spans_len; i++) {
        span_free_contents(&a->spans[i]);
    }

    free(a->spans);
    free(a);
}

bool annotation_add_span(Annotation *a, size_t start, size_t end,
                         const char *layer, const char *key, const char *value) {
    if (!a || start >= end) {
        return false;
    }

    // Grow array if needed
    if (a->spans_len >= a->spans_cap) {
        size_t new_cap = a->spans_cap * 2;
        Span *new_spans = realloc(a->spans, new_cap * sizeof(Span));
        if (!new_spans) {
            return false;
        }
        a->spans = new_spans;
        a->spans_cap = new_cap;
    }

    Span *span = &a->spans[a->spans_len];
    span->start = start;
    span->end = end;
    span->layer = strdup_safe(layer);
    span->key = strdup_safe(key);
    span->value = strdup_safe(value);

    // Check allocations succeeded for non-null inputs
    if ((layer && !span->layer) || (key && !span->key) || (value && !span->value)) {
        span_free_contents(span);
        return false;
    }

    a->spans_len++;
    return true;
}

void annotation_clear(Annotation *a) {
    if (!a) {
        return;
    }

    for (size_t i = 0; i < a->spans_len; i++) {
        span_free_contents(&a->spans[i]);
    }

    a->spans_len = 0;
}

size_t annotation_spans_at(const Annotation *a, size_t pos, Span **out, size_t out_cap) {
    if (!a || !out || out_cap == 0) {
        return 0;
    }

    size_t found = 0;

    for (size_t i = 0; i < a->spans_len && found < out_cap; i++) {
        const Span *span = &a->spans[i];
        if (pos >= span->start && pos < span->end) {
            out[found] = (Span *)span;
            found++;
        }
    }

    return found;
}

size_t annotation_spans_in_range(const Annotation *a, size_t start, size_t end,
                                  Span **out, size_t out_cap) {
    if (!a || !out || out_cap == 0 || start >= end) {
        return 0;
    }

    size_t found = 0;

    for (size_t i = 0; i < a->spans_len && found < out_cap; i++) {
        const Span *span = &a->spans[i];
        // Check for overlap: span overlaps [start, end) if span.start < end && span.end > start
        if (span->start < end && span->end > start) {
            out[found] = (Span *)span;
            found++;
        }
    }

    return found;
}
