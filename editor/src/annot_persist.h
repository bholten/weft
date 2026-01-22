#ifndef WEFT_ANNOT_PERSIST_H
#define WEFT_ANNOT_PERSIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "annot_store.h"
#include "document.h"

// Context window size for before/after text
#define CONTEXT_WINDOW_SIZE 32

// Document ID length (UUID string + null terminator)
#define DOC_ID_LEN 37

// === Context Anchor ===
// Robust position storage that can re-locate after document edits

typedef struct {
  size_t approx_pos;    // Approximate byte offset
  char *before_ctx;     // Text before position (up to CONTEXT_WINDOW_SIZE bytes)
  size_t before_len;    // Length of before context
  char *after_ctx;      // Text after position (up to CONTEXT_WINDOW_SIZE bytes)
  size_t after_len;     // Length of after context
  AnchorBias bias;      // LEFT or RIGHT
} ContextAnchor;

// === Registry Entry ===
// Maps a file path to a persistent document ID

typedef struct {
  char *file_path;        // Absolute canonical path (owned)
  char id[DOC_ID_LEN];    // UUID string
  uint64_t last_modified; // File mtime for staleness detection
} RegistryEntry;

// === Document Registry ===
// Manages path -> doc_id mappings

typedef struct DocRegistry {
  RegistryEntry *entries;
  size_t count;
  size_t capacity;
  char *data_dir;         // ~/.local/share/weft (owned)
  bool dirty;             // Needs to be saved
} DocRegistry;

// === Persisted Annotation ===
// Annotation record stored on disk

typedef struct {
  ContextAnchor start;
  ContextAnchor end;
  char *layer;            // Layer name (owned)
  char **keys;            // Metadata keys (owned array of owned strings)
  char **values;          // Metadata values (owned array of owned strings)
  size_t meta_count;      // Number of key/value pairs
} PersistedAnnot;

// === Re-anchoring Result ===
// Status of re-anchoring an annotation after document changes

typedef enum {
  ANCHOR_OK,        // Exact context match found
  ANCHOR_FUZZY,     // Context partially matched (position approximate)
  ANCHOR_ORPHANED   // Could not locate context (needs user intervention)
} AnchorStatus;

typedef struct {
  PersistedAnnot *annot;  // The original persisted annotation (not owned)
  size_t new_start;       // Resolved start position
  size_t new_end;         // Resolved end position
  AnchorStatus status;    // How confident we are in the position
  int confidence;         // 0-100 confidence score
} ReanchorResult;

// === UUID Generation ===

// Generate a new UUID v4 string (uses /dev/urandom)
void doc_generate_uuid(char out[DOC_ID_LEN]);

// === Context Anchor Operations ===

// Create a context anchor from a document position
ContextAnchor context_anchor_create(const Document *doc, size_t pos,
                                    AnchorBias bias);

// Free a context anchor's allocated memory
void context_anchor_free(ContextAnchor *ca);

// Resolve a context anchor to a position in the (possibly modified) document
// max_search_radius: how far to search from approx_pos (0 = entire document)
// confidence_out: receives confidence score 0-100 (can be NULL)
// Returns the resolved position, or SIZE_MAX if not found
size_t context_anchor_resolve(const ContextAnchor *ca, const Document *doc,
                              size_t max_search_radius, int *confidence_out);

// === Persisted Annotation Operations ===

// Free a persisted annotation's allocated memory
void persisted_annot_free(PersistedAnnot *pa);

// Free an array of persisted annotations
void persisted_annot_array_free(PersistedAnnot *annots, size_t count);

// === Document Registry Operations ===

// Create a new document registry
DocRegistry *doc_registry_new(const char *data_dir);

// Free a document registry
void doc_registry_free(DocRegistry *reg);

// Save registry to disk (registry.weft)
bool doc_registry_save(DocRegistry *reg);

// Load registry from disk
bool doc_registry_load(DocRegistry *reg);

// Get or create a document ID for a file path
// Returns pointer to the ID string (owned by registry, do not free)
// Creates a new entry if path not found
const char *doc_registry_get_or_create(DocRegistry *reg, const char *path);

// Get document ID for a path (returns NULL if not found)
const char *doc_registry_get(const DocRegistry *reg, const char *path);

// Update the last_modified time for a path
void doc_registry_update_mtime(DocRegistry *reg, const char *path,
                               uint64_t mtime);

// === Annotation Serialization ===

// Save annotations from an AnnotStore to disk
// Filters to only include annotations in persistent layers
bool annot_persist_save(const AnnotStore *store, const Document *doc,
                        const char *doc_id, const char *data_dir,
                        const char *const *persistent_layers,
                        size_t persistent_layer_count);

// Load annotations from disk
// Returns array of persisted annotations (caller must free with
// persisted_annot_array_free)
// Returns NULL on error, count is set to 0
PersistedAnnot *annot_persist_load(const char *doc_id, const char *data_dir,
                                   size_t *out_count);

// === Re-anchoring ===

// Re-anchor persisted annotations to a document
// Returns array of results (caller must free with free())
ReanchorResult *annot_persist_reanchor(const PersistedAnnot *annots,
                                       size_t count, const Document *doc,
                                       size_t *out_count);

// Restore re-anchored annotations into an AnnotStore
// Returns number of annotations restored (excludes orphaned)
size_t annot_persist_restore(AnnotStore *store, const ReanchorResult *results,
                             size_t count);

// === Utility Functions ===

// Get XDG data directory for an app (creates if needed)
// Returns allocated string (caller must free), or NULL on error
char *get_xdg_data_dir(const char *app_name);

// Create directory and parents if needed
// Returns true on success or if directory already exists
bool mkdir_recursive(const char *path);

// Join two path components
// Returns allocated string (caller must free)
char *path_join(const char *dir, const char *filename);

// Get canonical (absolute, resolved) path
// Returns allocated string (caller must free), or NULL on error
char *path_canonical(const char *path);

// Get file modification time (returns 0 on error)
uint64_t get_file_mtime(const char *path);

#endif // WEFT_ANNOT_PERSIST_H
