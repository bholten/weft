#ifndef WEFT_DECORATION_H
#define WEFT_DECORATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#include "interaction.h"

// Decoration: ephemeral render output from a layer
// Unlike persistent annotations, decorations are recomputed freely
// and do not survive document edits.
typedef struct {
    size_t start;             // Start byte offset
    size_t end;               // End byte offset (exclusive)

    // Style attributes
    SDL_Color fg;
    SDL_Color bg;
    bool has_fg;
    bool has_bg;
    bool underline;

    // Action (optional) - script to execute on interaction
    InteractType action_type; // Which event triggers this action
    char *action_script;      // Lcl script to execute (owned, NULL if none)

    // Metadata (optional) - arbitrary key/value pairs
    char **meta_keys;         // Keys (owned array of owned strings)
    char **meta_values;       // Values (owned array of owned strings)
    size_t meta_count;

    // Provenance (for inspection)
    const char *layer_name;   // Which layer produced this (not owned, points to layer)
} Decoration;

// === Decoration Lifecycle ===

// Initialize a decoration (zeroes all fields)
void decoration_init(Decoration *d);

// Free resources owned by a decoration (does not free the struct itself)
void decoration_cleanup(Decoration *d);

// Set the action for a decoration (takes ownership of script)
void decoration_set_action(Decoration *d, InteractType type, char *script);

// Add metadata to a decoration (copies key and value)
void decoration_add_meta(Decoration *d, const char *key, const char *value);

// Get metadata value by key (returns NULL if not found)
const char *decoration_get_meta(const Decoration *d, const char *key);

// === Decoration List ===

// Dynamic array of decorations for layer output
typedef struct {
    Decoration *items;
    size_t count;
    size_t capacity;
} DecorationList;

// Initialize a decoration list
void decoration_list_init(DecorationList *list);

// Free a decoration list and all its decorations
void decoration_list_free(DecorationList *list);

// Clear a decoration list (frees decorations but keeps capacity)
void decoration_list_clear(DecorationList *list);

// Add a decoration to the list (takes ownership)
// Returns pointer to the added decoration for further modification
Decoration *decoration_list_add(DecorationList *list, size_t start, size_t end);

// Query decorations overlapping a range
// Returns count of matching decorations, fills out_indices with their indices
size_t decoration_list_in_range(const DecorationList *list,
                                 size_t start, size_t end,
                                 size_t *out_indices, size_t max_indices);

// Query decorations at a specific position
size_t decoration_list_at(const DecorationList *list, size_t pos,
                           size_t *out_indices, size_t max_indices);

#endif // WEFT_DECORATION_H
