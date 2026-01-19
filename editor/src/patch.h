#ifndef WEFT_PATCH_H
#define WEFT_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct Buffer Buffer;
typedef struct document Document;  // Match document.h struct tag

// === PatchEdit ===
// A single edit operation: replace [start, end) with insert text

typedef struct PatchEdit {
    size_t start;           // Start offset in original document
    size_t end;             // End offset in original document (start == end for pure insert)
    char *insert;           // Text to insert (NULL or "" for pure delete)
    size_t insert_len;      // Length of insert text

    // Optional safety check
    char *expected_text;    // Expected text at [start, end) before applying (NULL = no check)
} PatchEdit;

// Create a patch edit (copies the insert string)
PatchEdit *patch_edit_new(size_t start, size_t end, const char *insert);

// Create a patch edit with expected text check
PatchEdit *patch_edit_new_with_check(size_t start, size_t end, const char *insert,
                                      const char *expected);

// Free a patch edit
void patch_edit_free(PatchEdit *edit);

// === Patch ===
// A collection of edits with metadata, representing a proposed transformation

typedef struct Patch {
    // Edit list
    PatchEdit **edits;
    size_t edit_count;
    size_t edit_capacity;

    // Metadata
    char *transform_name;   // Name of the transform that created this patch
    char *description;      // Human-readable description
    char *origin;           // Where this patch came from (script path, etc.)

    // Target info (for validation)
    uint64_t target_doc_rev;    // Document revision this patch was created against
    size_t target_doc_len;      // Document length when patch was created
} Patch;

// Create an empty patch
Patch *patch_new(const char *transform_name);

// Free a patch and all its edits
void patch_free(Patch *patch);

// Add an edit to the patch (takes ownership of edit)
bool patch_add_edit(Patch *patch, PatchEdit *edit);

// Convenience: add an edit directly
bool patch_add(Patch *patch, size_t start, size_t end, const char *insert);

// Set patch metadata
void patch_set_description(Patch *patch, const char *description);
void patch_set_origin(Patch *patch, const char *origin);
void patch_set_target(Patch *patch, uint64_t doc_rev, size_t doc_len);

// Sort edits by start position (required before applying)
void patch_sort_edits(Patch *patch);

// Check if edits are non-overlapping and in order
bool patch_is_valid(const Patch *patch);

// === Patch Application ===

// Apply result codes
typedef enum {
    PATCH_OK = 0,               // Patch applied successfully
    PATCH_ERR_INVALID,          // Patch edits overlap or are out of order
    PATCH_ERR_OUT_OF_BOUNDS,    // Edit range exceeds document bounds
    PATCH_ERR_MISMATCH,         // Expected text didn't match
    PATCH_ERR_DOC_CHANGED,      // Document revision changed since patch creation
} PatchResult;

// Validate a patch against a document (does not apply)
PatchResult patch_validate(const Patch *patch, const Document *doc);

// Apply a patch to a buffer (creates one undo step)
// Edits are applied in reverse order to maintain offsets
PatchResult patch_apply(const Patch *patch, Buffer *buffer);

// === Patch Preview (Diff Generation) ===

// Generate a unified diff string from a patch
// Returns malloc'd string, caller must free
char *patch_to_diff(const Patch *patch, const Document *doc, int context_lines);

// Get statistics about a patch
void patch_stats(const Patch *patch, size_t *out_additions, size_t *out_deletions,
                 size_t *out_edit_count);

#endif // WEFT_PATCH_H
