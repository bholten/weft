#ifndef WEFT_TRANSFORM_H
#define WEFT_TRANSFORM_H

#include <stdbool.h>
#include <stddef.h>

#include "patch.h"

// Forward declarations
typedef struct Buffer Buffer;
typedef struct LclHost LclHost;

// === Transform ===
// A transform is a named operation that computes a Patch from a buffer range.
// Transforms are registered by name and can be invoked from Lcl or the UI.

// Transform function signature
// Parameters:
//   buffer    - The buffer to transform
//   start     - Start of the range to transform
//   end       - End of the range (exclusive)
//   args      - Optional transform-specific arguments (NULL-terminated array, can be NULL)
//   user_data - User data passed at registration
// Returns:
//   A newly allocated Patch, or NULL on error
//   Caller takes ownership of the returned Patch
typedef Patch *(*TransformFn)(Buffer *buffer, size_t start, size_t end,
                               const char **args, void *user_data);

// Transform cleanup function (optional)
typedef void (*TransformCleanupFn)(void *user_data);

// === Transform Registry ===

// Initialize the transform registry
void transform_registry_init(void);

// Cleanup the transform registry
void transform_registry_cleanup(void);

// Register a transform
// Parameters:
//   name        - Unique name for the transform (e.g., "upper", "format")
//   description - Human-readable description
//   fn          - The transform function
//   cleanup     - Optional cleanup function for user_data (NULL if not needed)
//   user_data   - Arbitrary data passed to fn
// Returns true on success, false if name already registered or allocation failed
bool transform_register(const char *name, const char *description,
                        TransformFn fn, TransformCleanupFn cleanup, void *user_data);

// Unregister a transform by name
// Calls the cleanup function if one was provided
void transform_unregister(const char *name);

// Look up a transform by name
// Returns the TransformFn, or NULL if not found
TransformFn transform_lookup(const char *name);

// Get transform description
// Returns NULL if transform not found
const char *transform_get_description(const char *name);

// Get list of registered transform names
// Returns number of transforms, fills out_names up to max_names
// out_names should be freed by caller (the strings are not copied)
size_t transform_list(const char **out_names, size_t max_names);

// === Transform Execution ===

// Run a transform by name
// Parameters:
//   name   - Name of the registered transform
//   buffer - Buffer to transform
//   start  - Start of range
//   end    - End of range (exclusive)
//   args   - Optional arguments (NULL-terminated array, can be NULL)
// Returns:
//   Newly allocated Patch, or NULL if transform not found or failed
Patch *transform_run(const char *name, Buffer *buffer, size_t start, size_t end,
                     const char **args);

// === Lcl-Driven Transforms ===

// Register a transform that runs an Lcl script
// The script receives variables: $start, $end, $text (the range text)
// The script should return a patch-like structure (list of {start end insert} lists)
bool transform_register_lcl(const char *name, const char *description,
                            LclHost *host, const char *script);

// === Built-in Transforms ===

// Register the built-in transforms (upper, lower, etc.)
void transform_register_builtins(void);

// Individual built-in transforms (exposed for direct use)
Patch *transform_upper(Buffer *buffer, size_t start, size_t end,
                       const char **args, void *user_data);
Patch *transform_lower(Buffer *buffer, size_t start, size_t end,
                       const char **args, void *user_data);
Patch *transform_reverse_lines(Buffer *buffer, size_t start, size_t end,
                               const char **args, void *user_data);
Patch *transform_sort_lines(Buffer *buffer, size_t start, size_t end,
                            const char **args, void *user_data);

#endif // WEFT_TRANSFORM_H
