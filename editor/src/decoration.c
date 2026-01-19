#include "decoration.h"

#include <stdlib.h>
#include <string.h>

// === Decoration Lifecycle ===

void decoration_init(Decoration *d) {
    memset(d, 0, sizeof(Decoration));
}

void decoration_cleanup(Decoration *d) {
    free(d->action_script);
    d->action_script = NULL;

    for (size_t i = 0; i < d->meta_count; i++) {
        free(d->meta_keys[i]);
        free(d->meta_values[i]);
    }
    free(d->meta_keys);
    free(d->meta_values);
    d->meta_keys = NULL;
    d->meta_values = NULL;
    d->meta_count = 0;
}

void decoration_set_action(Decoration *d, InteractType type, char *script) {
    free(d->action_script);
    d->action_type = type;
    d->action_script = script;  // Takes ownership
}

void decoration_add_meta(Decoration *d, const char *key, const char *value) {
    size_t new_count = d->meta_count + 1;

    char **new_keys = realloc(d->meta_keys, new_count * sizeof(char *));
    char **new_values = realloc(d->meta_values, new_count * sizeof(char *));

    if (!new_keys || !new_values) {
        free(new_keys);
        free(new_values);
        return;  // Allocation failed, leave unchanged
    }

    d->meta_keys = new_keys;
    d->meta_values = new_values;

    d->meta_keys[d->meta_count] = strdup(key);
    d->meta_values[d->meta_count] = strdup(value);
    d->meta_count = new_count;
}

const char *decoration_get_meta(const Decoration *d, const char *key) {
    for (size_t i = 0; i < d->meta_count; i++) {
        if (strcmp(d->meta_keys[i], key) == 0) {
            return d->meta_values[i];
        }
    }
    return NULL;
}

// === Decoration List ===

#define DECORATION_LIST_INITIAL_CAPACITY 16

void decoration_list_init(DecorationList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void decoration_list_free(DecorationList *list) {
    decoration_list_clear(list);
    free(list->items);
    list->items = NULL;
    list->capacity = 0;
}

void decoration_list_clear(DecorationList *list) {
    for (size_t i = 0; i < list->count; i++) {
        decoration_cleanup(&list->items[i]);
    }
    list->count = 0;
}

static bool decoration_list_grow(DecorationList *list) {
    size_t new_cap = list->capacity == 0
                         ? DECORATION_LIST_INITIAL_CAPACITY
                         : list->capacity * 2;

    Decoration *new_items = realloc(list->items, new_cap * sizeof(Decoration));
    if (!new_items) {
        return false;
    }

    list->items = new_items;
    list->capacity = new_cap;
    return true;
}

Decoration *decoration_list_add(DecorationList *list, size_t start, size_t end) {
    if (list->count >= list->capacity) {
        if (!decoration_list_grow(list)) {
            return NULL;
        }
    }

    Decoration *d = &list->items[list->count];
    decoration_init(d);
    d->start = start;
    d->end = end;
    list->count++;

    return d;
}

size_t decoration_list_in_range(const DecorationList *list,
                                 size_t start, size_t end,
                                 size_t *out_indices, size_t max_indices) {
    size_t found = 0;

    for (size_t i = 0; i < list->count && found < max_indices; i++) {
        const Decoration *d = &list->items[i];
        // Check for overlap: decoration overlaps [start, end) if
        // d->start < end && d->end > start
        if (d->start < end && d->end > start) {
            out_indices[found++] = i;
        }
    }

    return found;
}

size_t decoration_list_at(const DecorationList *list, size_t pos,
                           size_t *out_indices, size_t max_indices) {
    size_t found = 0;

    for (size_t i = 0; i < list->count && found < max_indices; i++) {
        const Decoration *d = &list->items[i];
        // Check if pos is within [d->start, d->end)
        if (pos >= d->start && pos < d->end) {
            out_indices[found++] = i;
        }
    }

    return found;
}
