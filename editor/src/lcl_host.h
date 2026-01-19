#ifndef WEFT_LCL_HOST_H
#define WEFT_LCL_HOST_H

#include <stdbool.h>
#include <stddef.h>

// Forward declarations
typedef struct lcl_interp lcl_interp;
typedef struct Buffer Buffer;
typedef struct Editor Editor;
typedef struct InteractEvent InteractEvent;
typedef struct BufferManager BufferManager;
typedef struct LayoutNode LayoutNode;
typedef struct Pane Pane;
typedef struct Patch Patch;

// Interaction type enum (mirrors InteractType from interaction.h)
typedef enum {
  HANDLER_LEFT_CLICK,
  HANDLER_MIDDLE_CLICK,
  HANDLER_RIGHT_CLICK,
  HANDLER_HOVER
} HandlerEventType;

// Registered handler: a global event handler script
typedef struct {
  char *name;                  // Handler identifier
  HandlerEventType event_type; // Which events trigger this
  char *script;                // Lcl script code
  int priority;                // Higher = tried first
  bool enabled;
} RegisteredHandler;

// Registered layer: a layer script that can be auto-run
typedef struct {
  char *name;        // Layer name (e.g., "syntax")
  char *script_path; // Path to .lcl file
  bool enabled;      // Whether auto-run is enabled
} RegisteredLayer;

// File type association: maps file extension to layer(s)
typedef struct {
  char *extension;   // File extension (e.g., ".c", ".py")
  char *layer_name;  // Layer name to register (e.g., "style")
  char *script_path; // Path to layer script
} FileTypeAssoc;

// Indent settings: maps file extension to indentation preferences
typedef struct {
  char *extension; // File extension (e.g., ".c", ".py")
  int tab_size;    // Tab width / spaces per indent level
  bool use_tabs;   // true = insert tabs, false = insert spaces
} IndentSettings;

// Registered keybinding: maps a key combination to an Lcl script
typedef struct {
  int keycode;   // SDL_Keycode (e.g., SDLK_W)
  int modifiers; // Modifier flags: KEYBIND_CTRL, KEYBIND_SHIFT, KEYBIND_ALT
  char *script;  // Lcl script to execute
  bool enabled;
} RegisteredKeybind;

// Keybinding modifier flags
#define KEYBIND_CTRL 0x01
#define KEYBIND_SHIFT 0x02
#define KEYBIND_ALT 0x04

// Lcl host context - manages the Lcl interpreter for Weft
typedef struct LclHost {
  lcl_interp *interp;     // The Lcl interpreter
  Buffer *current_buffer; // Current buffer for bindings (set during layer
                          // execution)

  // Registered layers for auto-triggering
  RegisteredLayer *registered_layers;
  size_t registered_layer_count;
  size_t registered_layer_cap;

  // Registered handlers for interaction events
  RegisteredHandler *handlers;
  size_t handler_count;
  size_t handler_cap;

  // Current interaction context (set during handler execution)
  InteractEvent *current_event;
  Editor *current_editor;

  // Current script path (set during layer execution for provenance tracking)
  char *current_script_path;

  // Buffer manager and layout for buffer operations
  BufferManager *buffer_manager;
  LayoutNode **layout_root_ptr; // Pointer to layout root (for adding panes)
  Pane **focused_pane_ptr; // Pointer to focused pane (for setting focus on new
                           // panes)
  void *app_context;       // Opaque pointer to app context

  // Renderer metrics for creating panes
  int char_width;
  int line_height;

  // Window dimensions for layout computation
  int window_width;
  int window_height;

  // File type associations
  FileTypeAssoc *filetypes;
  size_t filetype_count;
  size_t filetype_cap;

  // Indent settings per file type
  IndentSettings *indent_settings;
  size_t indent_count;
  size_t indent_cap;

  // Keybindings
  RegisteredKeybind *keybindings;
  size_t keybind_count;
  size_t keybind_cap;

  // Config directory path
  char *config_dir;

  // Pending patch for transform preview
  Patch *pending_patch;
  Buffer *patch_target_buffer;

  // REPL attachment - when set, doc commands in REPL operate on this buffer
  Buffer *repl_attached_buffer;
} LclHost;

// === Lifecycle ===

// Create and initialize the Lcl host with Weft bindings
LclHost *lcl_host_new(void);

// Free the Lcl host and interpreter
void lcl_host_free(LclHost *host);

// === Evaluation ===

// Evaluate a string of Lcl code
// Returns true on success, false on error
// If error_msg is non-NULL, it receives a pointer to the error message (do not
// free)
bool lcl_host_eval(LclHost *host, const char *code, const char **error_msg);

// Evaluate an Lcl file
bool lcl_host_eval_file(LclHost *host, const char *path,
                        const char **error_msg);

// Evaluate Lcl code and return the result as a string
// Returns allocated string on success (caller must free), NULL on error
// If error_msg is non-NULL, it receives a pointer to the error message on
// failure
char *lcl_host_eval_to_string(LclHost *host, const char *code,
                              const char **error_msg);

// === Buffer Context ===

// Set the current buffer for bindings (used during layer execution)
void lcl_host_set_buffer(LclHost *host, Buffer *buffer);

// Get the current buffer
Buffer *lcl_host_get_buffer(LclHost *host);

// === Layer Execution ===

// Execute a layer script on the current buffer
// The layer name is used for clearing previous annotations from this layer
// Returns true on success, false on error
bool lcl_host_run_layer(LclHost *host, const char *layer_name,
                        const char *script, const char **error_msg);

// Execute a layer script file
bool lcl_host_run_layer_file(LclHost *host, const char *layer_name,
                             const char *path, const char **error_msg);

// === Layer Registration (for auto-triggering) ===

// Register a layer script for auto-run when layers become dirty
// The layer will be run against the current buffer when refresh is called
void lcl_host_register_layer(LclHost *host, const char *name,
                             const char *script_path);

// Unregister a layer by name
void lcl_host_unregister_layer(LclHost *host, const char *name);

// Enable or disable a registered layer
void lcl_host_set_layer_enabled(LclHost *host, const char *name, bool enabled);

// Refresh all dirty registered layers
// Returns the number of layers that were refreshed
int lcl_host_refresh_dirty_layers(LclHost *host);

// Check if any registered layers are dirty
bool lcl_host_has_dirty_layers(LclHost *host);

// === Interaction Handler Registration ===

// Register a global handler for interaction events
// Priority: higher values are tried first (default 0)
void lcl_host_register_handler(LclHost *host, const char *name,
                               HandlerEventType event_type, const char *script,
                               int priority);

// Unregister a handler by name
void lcl_host_unregister_handler(LclHost *host, const char *name);

// Enable or disable a registered handler
void lcl_host_set_handler_enabled(LclHost *host, const char *name,
                                  bool enabled);

// === Interaction Context ===

// Set current interaction event context (called before executing handlers)
void lcl_host_set_current_event(LclHost *host, InteractEvent *event,
                                Editor *editor);

// Clear current interaction event context
void lcl_host_clear_current_event(LclHost *host);

// Dispatch global handlers for an event
// Returns true if any handler marked the event as handled
bool lcl_host_dispatch_global_handlers(LclHost *host, InteractEvent *event,
                                       Editor *editor);

// === App Context ===

// Set the buffer manager and layout root for buffer operations
void lcl_host_set_app_context(LclHost *host, BufferManager *bm,
                              LayoutNode **layout_root, Pane **focused_pane,
                              void *app_ctx, int char_width, int line_height,
                              int window_width, int window_height);

// Update window dimensions (call on resize)
void lcl_host_set_window_size(LclHost *host, int width, int height);

// === Configuration ===

// Load configuration from the default config file (~/.config/weft/init.lcl)
// Returns true if config was loaded successfully (or no config file exists)
bool lcl_host_load_config(LclHost *host);

// Register a file type association (extension -> layer)
void lcl_host_register_filetype(LclHost *host, const char *extension,
                                const char *layer_name,
                                const char *script_path);

// Apply matching layers to a buffer based on its file path
// Call this after opening a buffer to auto-load syntax highlighting, etc.
void lcl_host_apply_filetypes(LclHost *host, Buffer *buffer);

// Apply indent settings to an editor based on its buffer's file extension
void lcl_host_apply_indent(LclHost *host, Editor *editor);

// Register indent settings for a file extension
void lcl_host_register_indent(LclHost *host, const char *extension,
                              int tab_size, bool use_tabs);

// Get indent settings for a file extension (returns NULL if not found)
const IndentSettings *lcl_host_get_indent(LclHost *host, const char *extension);

// Get the config directory path
const char *lcl_host_get_config_dir(LclHost *host);

// === Keybinding Registration ===

// Register a keybinding (modifiers: KEYBIND_CTRL, KEYBIND_SHIFT, KEYBIND_ALT)
void lcl_host_register_keybind(LclHost *host, int keycode, int modifiers,
                               const char *script);

// Try to dispatch a keybinding for a key event
// Returns true if a keybinding was found and executed
bool lcl_host_dispatch_keybind(LclHost *host, int keycode, int modifiers);

// === Accessors for interaction dispatch ===

// Get the buffer manager
BufferManager *lcl_host_get_buffer_manager(LclHost *host);

// Get the REPL attached buffer (NULL if not attached)
Buffer *lcl_host_get_repl_attached_buffer(LclHost *host);

// Get the layout root (for finding panes)
LayoutNode *lcl_host_get_layout_root(LclHost *host);

#endif // WEFT_LCL_HOST_H
