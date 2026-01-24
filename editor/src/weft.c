#define SDL_MAIN_USE_CALLBACKS

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "annot_persist.h"
#include "annot_store.h"
#include "buffer.h"
#include "document.h"
#include "editor.h"
#include "input.h"
#include "interaction.h"
#include "layout.h"
#include "lcl_host.h"
#include "pane.h"
#include "platform.h"
#include "renderer.h"
#include "tree_sitter_hl.h"
#include "weft.h"

// Embedded default font (generated at build time)
#include "embedded_font.h"

// Application context using our new modules
typedef struct {
  WeftApp app;
  InputState input_state;
} AppContext;

SDL_AppResult SDL_AppInit(void **app_state, int argc, char **argv) {
  // Create application context
  AppContext *ctx = calloc(1, sizeof(AppContext));
  if (!ctx) {
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Failed to allocate context");
    return SDL_APP_FAILURE;
  }

  // Initialize platform (creates window and SDL renderer)
  ctx->app.platform = platform_new("weft", 800, 600);
  if (!ctx->app.platform) {
    free(ctx);
    return SDL_APP_FAILURE;
  }

  // Initialize renderer (handles fonts and drawing)
  ctx->app.renderer = renderer_new(ctx->app.platform);
  if (!ctx->app.renderer) {
    platform_free(ctx->app.platform);
    free(ctx);
    return SDL_APP_FAILURE;
  }

  // Load embedded default font initially
  // (Will be replaced by configured font after config is loaded)
#if EMBEDDED_FONT_AVAILABLE
  if (!renderer_load_font_from_memory(ctx->app.renderer, embedded_font_data,
                                      embedded_font_data_len, 14)) {
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Failed to load embedded font");
    renderer_free(ctx->app.renderer);
    platform_free(ctx->app.platform);
    free(ctx);
    return SDL_APP_FAILURE;
  }
#else
  // No embedded font - try system fallback
  SDL_LogWarn(SDL_LOG_CATEGORY_CUSTOM,
              "No embedded font available, trying system fonts");
  bool font_loaded = false;
  const char *fallback_fonts[] = {
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
      "/System/Library/Fonts/Menlo.ttc", NULL};
  for (const char **fp = fallback_fonts; *fp; fp++) {
    if (renderer_load_font(ctx->app.renderer, *fp, 14)) {
      font_loaded = true;
      break;
    }
  }
  if (!font_loaded) {
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Failed to load any font");
    renderer_free(ctx->app.renderer);
    platform_free(ctx->app.platform);
    free(ctx);
    return SDL_APP_FAILURE;
  }
#endif

  // Create buffer manager
  ctx->app.buffers = buffer_manager_new();
  if (!ctx->app.buffers) {
    renderer_free(ctx->app.renderer);
    platform_free(ctx->app.platform);
    free(ctx);
    return SDL_APP_FAILURE;
  }

  // Create initial buffer (load file or open REPL)
  Buffer *initial_buffer = NULL;
  bool started_with_repl = false;
  if (argc > 1) {
    initial_buffer = buffer_manager_open_file(ctx->app.buffers, argv[1]);
    if (!initial_buffer) {
      SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Failed to load file: %s", argv[1]);
    }
  }
  if (!initial_buffer) {
    // No file specified - create REPL buffer
    initial_buffer = buffer_manager_create(ctx->app.buffers);
    if (initial_buffer) {
      const char *welcome =
          ";; Lcl REPL - Weft Scripting Console\n"
          ";; Type Lcl expressions and press Ctrl+Enter to evaluate\n"
          ";; Results appear below each expression\n"
          ";;\n"
          ";; Examples:\n"
          ";;   [+ 1 2]              ;; => 3\n"
          ";;   [weft::doc::length]  ;; => document length\n"
          ";;   [weft::version]      ;; => version string\n"
          ";;\n\n";
      document_insert(initial_buffer->document, 0, welcome, strlen(welcome));
      initial_buffer->file_path = strdup("*lcl*");
      started_with_repl = true;
    }
  }
  if (!initial_buffer) {
    buffer_manager_free(ctx->app.buffers);
    renderer_free(ctx->app.renderer);
    platform_free(ctx->app.platform);
    free(ctx);
    return SDL_APP_FAILURE;
  }

  // Create Lcl scripting host
  ctx->app.lcl = lcl_host_new();
  if (!ctx->app.lcl) {
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Failed to create Lcl host");
    buffer_manager_free(ctx->app.buffers);
    renderer_free(ctx->app.renderer);
    platform_free(ctx->app.platform);
    free(ctx);
    return SDL_APP_FAILURE;
  }

  // Set the initial buffer for Lcl bindings
  lcl_host_set_buffer(ctx->app.lcl, initial_buffer);

  // Set up app context for buffer operations
  int char_width = renderer_get_char_width(ctx->app.renderer);
  int line_height = renderer_get_line_height(ctx->app.renderer);
  int win_w, win_h;
  SDL_GetWindowSize(platform_get_window(ctx->app.platform), &win_w, &win_h);
  lcl_host_set_app_context(ctx->app.lcl, ctx->app.buffers,
                           &ctx->app.layout_root, &ctx->app.focused_pane, ctx,
                           char_width, line_height, win_w, win_h);

  // Load user configuration (~/.config/weft/init.lcl)
  if (!lcl_host_load_config(ctx->app.lcl)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_CUSTOM, "Failed to load config file");
  }

  // Apply configured font if specified in config
  if (lcl_host_has_font_config(ctx->app.lcl)) {
    const FontConfig *font_cfg = lcl_host_get_font(ctx->app.lcl);
    if (font_cfg && font_cfg->path) {
      if (renderer_load_font(ctx->app.renderer, font_cfg->path,
                             font_cfg->size)) {
        SDL_Log("Loaded configured font: %s (size %d)", font_cfg->path,
                font_cfg->size);
        // Update metrics after font change
        char_width = renderer_get_char_width(ctx->app.renderer);
        line_height = renderer_get_line_height(ctx->app.renderer);
        lcl_host_set_app_context(ctx->app.lcl, ctx->app.buffers,
                                 &ctx->app.layout_root, &ctx->app.focused_pane,
                                 ctx, char_width, line_height, win_w, win_h);
      } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_CUSTOM,
                    "Failed to load configured font: %s, using default",
                    font_cfg->path);
      }
    }
  }

  // Apply file type associations to initial buffer (syntax highlighting, etc.)
  lcl_host_apply_filetypes(ctx->app.lcl, initial_buffer);

  // Load persisted annotations for initial buffer (if it has a file path)
  if (initial_buffer->file_path && initial_buffer->file_path[0] != '*') {
    DocRegistry *reg = lcl_host_get_doc_registry(ctx->app.lcl);
    const char *data_dir = lcl_host_get_data_dir(ctx->app.lcl);
    if (reg && data_dir) {
      buffer_load_annotations(initial_buffer, reg, data_dir);
    }
  }

  // Note: Acme-like middle-click handler is now configured in
  // ~/.config/weft/init.lcl This allows users to customize the behavior without
  // recompiling.

  // Create initial pane with the buffer (uses char_width, line_height from
  // earlier)
  Pane *initial_pane = pane_new(initial_buffer, char_width, line_height);
  if (!initial_pane) {
    buffer_manager_free(ctx->app.buffers);
    renderer_free(ctx->app.renderer);
    platform_free(ctx->app.platform);
    free(ctx);
    return SDL_APP_FAILURE;
  }

  // Apply viewlayer configuration based on file type
  lcl_host_apply_viewlayers(ctx->app.lcl, initial_pane);

  // Apply indent settings based on file type
  lcl_host_apply_indent(ctx->app.lcl, pane_get_editor(initial_pane));

  // Apply wrap settings based on file type
  lcl_host_apply_wrap(ctx->app.lcl, pane_get_editor(initial_pane));

  // Create layout root with the pane
  ctx->app.layout_root = layout_leaf_new(initial_pane);
  if (!ctx->app.layout_root) {
    pane_free(initial_pane);
    buffer_manager_free(ctx->app.buffers);
    renderer_free(ctx->app.renderer);
    platform_free(ctx->app.platform);
    free(ctx);
    return SDL_APP_FAILURE;
  }

  // Set focused pane
  ctx->app.focused_pane = initial_pane;

  // Compute initial bounds
  SDL_Rect window_rect = {.x = 0, .y = 0, .w = 800, .h = 600};
  layout_compute_bounds(ctx->app.layout_root, window_rect);

  // Initialize input state
  input_state_init(&ctx->input_state);

  // Enable text input
  SDL_StartTextInput(platform_get_window(ctx->app.platform));

  ctx->app.running = true;
  *app_state = ctx;

  return SDL_APP_CONTINUE;
}

// Helper to create a new pane with a new buffer
static Pane *create_new_pane(AppContext *ctx) {
  Buffer *new_buffer = buffer_manager_create(ctx->app.buffers);
  if (!new_buffer) {
    return NULL;
  }

  int char_width = renderer_get_char_width(ctx->app.renderer);
  int line_height = renderer_get_line_height(ctx->app.renderer);

  Pane *new_pane = pane_new(new_buffer, char_width, line_height);
  if (!new_pane) {
    buffer_manager_close(ctx->app.buffers, new_buffer);
    return NULL;
  }

  // Apply viewlayer configuration (will add default style layer for new buffer)
  lcl_host_apply_viewlayers(ctx->app.lcl, new_pane);

  return new_pane;
}

// Helper to get window bounds
static SDL_Rect get_window_rect(AppContext *ctx) {
  int w, h;
  SDL_GetWindowSize(platform_get_window(ctx->app.platform), &w, &h);
  return (SDL_Rect){.x = 0, .y = 0, .w = w, .h = h};
}

// File dialog callback data
typedef struct {
  AppContext *ctx;
  Pane *target_pane;
  bool save_as; // true for SaveAs, false for Open
} FileDialogData;

// Open file dialog callback
static void open_file_callback(void *userdata, const char *const *filelist,
                               int filter) {
  (void)filter;
  FileDialogData *data = (FileDialogData *)userdata;

  if (filelist && filelist[0]) {
    Buffer *buf = pane_get_buffer(data->target_pane);
    if (buf) {
      buffer_load(buf, filelist[0]);

      // Load persisted annotations for this file
      LclHost *lcl = data->ctx->app.lcl;
      DocRegistry *reg = lcl_host_get_doc_registry(lcl);
      const char *data_dir = lcl_host_get_data_dir(lcl);
      if (reg && data_dir) {
        buffer_load_annotations(buf, reg, data_dir);
      }
    }
  }

  free(data);
}

// Save file dialog callback
static void save_file_callback(void *userdata, const char *const *filelist,
                               int filter) {
  (void)filter;
  FileDialogData *data = (FileDialogData *)userdata;

  if (filelist && filelist[0]) {
    Buffer *buf = pane_get_buffer(data->target_pane);
    if (buf) {
      if (buffer_save_as(buf, filelist[0])) {
        // Save annotations after successfully saving the file
        LclHost *lcl = data->ctx->app.lcl;
        DocRegistry *reg = lcl_host_get_doc_registry(lcl);
        const char *data_dir = lcl_host_get_data_dir(lcl);
        if (reg && data_dir) {
          size_t layer_count = 0;
          const char *const *layers =
              lcl_host_get_persistent_layers(lcl, &layer_count);
          buffer_save_annotations(buf, reg, data_dir, layers, layer_count);
          doc_registry_save(reg);
        }
      }
    }
  }

  free(data);
}

// Handle tag bar command
static void handle_tag_command(AppContext *ctx, Pane *pane, TagCommand cmd) {
  switch (cmd) {
  case TAG_CMD_NEW: {
    // Create new buffer in current pane
    Buffer *buf = pane_get_buffer(pane);
    if (buf && buf->ref_count <= 1) {
      // If buffer has no other refs, just clear it
      document_delete(buf->document, 0, document_length(buf->document));
      free(buf->file_path);
      buf->file_path = NULL;
      buf->modified = false;
    }
    break;
  }

  case TAG_CMD_OPEN: {
    FileDialogData *data = calloc(1, sizeof(FileDialogData));
    if (data) {
      data->ctx = ctx;
      data->target_pane = pane;
      data->save_as = false;
      SDL_ShowOpenFileDialog(open_file_callback, data,
                             platform_get_window(ctx->app.platform),
                             NULL, // No filters
                             0,    // No filter count
                             NULL, // Default location
                             false // Single file
      );
    }
    break;
  }

  case TAG_CMD_SAVE: {
    Buffer *buf = pane_get_buffer(pane);
    if (buf) {
      if (buf->file_path) {
        if (buffer_save(buf)) {
          // Save annotations after successfully saving the file
          LclHost *lcl = ctx->app.lcl;
          DocRegistry *reg = lcl_host_get_doc_registry(lcl);
          const char *data_dir = lcl_host_get_data_dir(lcl);
          if (reg && data_dir) {
            size_t layer_count = 0;
            const char *const *layers =
                lcl_host_get_persistent_layers(lcl, &layer_count);
            buffer_save_annotations(buf, reg, data_dir, layers, layer_count);
            doc_registry_save(reg);
          }
        }
      } else {
        // No path - fall through to SaveAs
        FileDialogData *data = calloc(1, sizeof(FileDialogData));
        if (data) {
          data->ctx = ctx;
          data->target_pane = pane;
          data->save_as = true;
          SDL_ShowSaveFileDialog(save_file_callback, data,
                                 platform_get_window(ctx->app.platform), NULL,
                                 0, NULL);
        }
      }
    }
    break;
  }

  case TAG_CMD_SAVEAS: {
    FileDialogData *data = calloc(1, sizeof(FileDialogData));
    if (data) {
      data->ctx = ctx;
      data->target_pane = pane;
      data->save_as = true;
      SDL_ShowSaveFileDialog(save_file_callback, data,
                             platform_get_window(ctx->app.platform), NULL, 0,
                             NULL);
    }
    break;
  }

  case TAG_CMD_DEL: {
    // Close this pane (like Ctrl+W)
    Pane *next = layout_next_pane(ctx->app.layout_root, pane);
    if (next && next != pane) {
      ctx->app.layout_root = layout_close_pane(ctx->app.layout_root, pane);
      ctx->app.focused_pane = next;
      if (ctx->app.layout_root) {
        layout_compute_bounds(ctx->app.layout_root, get_window_rect(ctx));
      } else {
        ctx->app.running = false;
      }
    }
    break;
  }

  default: break;
  }
}

SDL_AppResult SDL_AppEvent(void *app_state, SDL_Event *event) {
  AppContext *ctx = (AppContext *)app_state;

  // Handle quit event directly (SDL callback model)
  if (event->type == SDL_EVENT_QUIT) {
    ctx->app.running = false;
    return SDL_APP_SUCCESS;
  }

  // Handle resize
  if (event->type == SDL_EVENT_WINDOW_RESIZED) {
    platform_update_size(ctx->app.platform);
    SDL_Rect window_rect = {
        .x = 0, .y = 0, .w = event->window.data1, .h = event->window.data2};
    layout_compute_bounds(ctx->app.layout_root, window_rect);
    // Update Lcl host with new window dimensions
    lcl_host_set_window_size(ctx->app.lcl, event->window.data1,
                             event->window.data2);
  }

  // Try custom keybindings for any key event
  if (event->type == SDL_EVENT_KEY_DOWN) {
    SDL_Keycode key = event->key.key;
    SDL_Keymod mod = event->key.mod;

    // Build modifier flags
    int keybind_mods = 0;
    if (mod & SDL_KMOD_CTRL) {
      keybind_mods |= KEYBIND_CTRL;
    }
    if (mod & SDL_KMOD_SHIFT) {
      keybind_mods |= KEYBIND_SHIFT;
    }
    if (mod & SDL_KMOD_ALT) {
      keybind_mods |= KEYBIND_ALT;
    }

    // Set current buffer/editor context for keybinding scripts
    if (ctx->app.focused_pane) {
      Editor *editor = pane_get_editor(ctx->app.focused_pane);
      Buffer *buffer = editor_get_buffer(editor);
      lcl_host_set_buffer(ctx->app.lcl, buffer);
      lcl_host_set_editor(ctx->app.lcl, editor);
    }

    if (lcl_host_dispatch_keybind(ctx->app.lcl, key, keybind_mods)) {
      // Recompute layout in case panes were added/removed
      layout_compute_bounds(ctx->app.layout_root, get_window_rect(ctx));
      return SDL_APP_CONTINUE;
    }
  }

  // Handle app-level key commands (split/close/focus)
  if (event->type == SDL_EVENT_KEY_DOWN && (event->key.mod & SDL_KMOD_CTRL)) {
    SDL_Keycode key = event->key.key;

    // Ctrl+2: Split horizontally
    if (key == SDLK_2) {
      Pane *new_pane = create_new_pane(ctx);
      if (new_pane) {
        LayoutNode *current_leaf =
            layout_find_pane(ctx->app.layout_root, ctx->app.focused_pane);
        if (current_leaf) {
          LayoutNode *new_split =
              layout_split_horizontal(current_leaf, new_pane);
          if (new_split && !new_split->parent) {
            ctx->app.layout_root = new_split;
          }
          layout_compute_bounds(ctx->app.layout_root, get_window_rect(ctx));
          ctx->app.focused_pane = new_pane;
        }
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+3: Split vertically
    if (key == SDLK_3) {
      Pane *new_pane = create_new_pane(ctx);
      if (new_pane) {
        LayoutNode *current_leaf =
            layout_find_pane(ctx->app.layout_root, ctx->app.focused_pane);
        if (current_leaf) {
          LayoutNode *new_split = layout_split_vertical(current_leaf, new_pane);
          if (new_split && !new_split->parent) {
            ctx->app.layout_root = new_split;
          }
          layout_compute_bounds(ctx->app.layout_root, get_window_rect(ctx));
          ctx->app.focused_pane = new_pane;
        }
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+F: Open search bar
    if (key == SDLK_F) {
      if (ctx->app.focused_pane) {
        pane_search_show(ctx->app.focused_pane);
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+H: Toggle replace mode
    if (key == SDLK_H) {
      if (ctx->app.focused_pane) {
        pane_search_toggle_replace(ctx->app.focused_pane);
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+G: Go to line
    if (key == SDLK_G) {
      if (ctx->app.focused_pane) {
        pane_goto_show(ctx->app.focused_pane);
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+Shift+W: Kill current buffer (destroy buffer completely)
    if (key == SDLK_W && (event->key.mod & SDL_KMOD_SHIFT)) {
      if (ctx->app.focused_pane && ctx->app.layout_root) {
        Buffer *buf = pane_get_buffer(ctx->app.focused_pane);
        if (buf) {
          // Build kill command with buffer ID
          char cmd[64];
          snprintf(cmd, sizeof(cmd), "weft::buffer::kill %u", buf->id);
          const char *error_msg = NULL;
          if (!lcl_host_eval(ctx->app.lcl, cmd, &error_msg)) {
            fprintf(stderr, "Failed to kill buffer: %s\n",
                    error_msg ? error_msg : "unknown");
          }
          // Update layout after potential pane closure
          if (ctx->app.layout_root) {
            layout_compute_bounds(ctx->app.layout_root, get_window_rect(ctx));
          } else {
            ctx->app.running = false;
            return SDL_APP_SUCCESS;
          }
        }
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+W: Close current pane (buffer survives)
    if (key == SDLK_W) {
      Pane *next =
          layout_next_pane(ctx->app.layout_root, ctx->app.focused_pane);
      if (next && next != ctx->app.focused_pane) {
        ctx->app.layout_root =
            layout_close_pane(ctx->app.layout_root, ctx->app.focused_pane);
        ctx->app.focused_pane = next;
        if (ctx->app.layout_root) {
          layout_compute_bounds(ctx->app.layout_root, get_window_rect(ctx));
        } else {
          // No panes left, quit
          ctx->app.running = false;
          return SDL_APP_SUCCESS;
        }
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+Tab: Cycle focus
    if (key == SDLK_TAB) {
      Pane *next =
          layout_next_pane(ctx->app.layout_root, ctx->app.focused_pane);
      if (next) {
        ctx->app.focused_pane = next;
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+W: Close current pane
    if (key == SDLK_W) {
      if (ctx->app.focused_pane && ctx->app.layout_root) {
        // Find the next pane to focus before closing
        Pane *next =
            layout_next_pane(ctx->app.layout_root, ctx->app.focused_pane);
        if (next == ctx->app.focused_pane) {
          next = NULL; // Only one pane, will become NULL
        }

        // Close the pane
        LayoutNode *new_root =
            layout_close_pane(ctx->app.layout_root, ctx->app.focused_pane);

        if (new_root) {
          // Update root and focus
          ctx->app.layout_root = new_root;
          ctx->app.focused_pane = next ? next : layout_first_pane(new_root);

          // Recompute layout bounds
          int win_w, win_h;
          SDL_GetWindowSize(platform_get_window(ctx->app.platform), &win_w,
                            &win_h);
          SDL_Rect available = {0, 0, win_w, win_h};
          layout_compute_bounds(ctx->app.layout_root, available);
        } else {
          // No panes left - exit
          ctx->app.running = false;
          return SDL_APP_SUCCESS;
        }
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+`: Open Lcl REPL
    if (key == SDLK_GRAVE) {
      const char *error_msg = NULL;
      if (!lcl_host_eval(ctx->app.lcl, "weft::repl::open", &error_msg)) {
        fprintf(stderr, "Failed to open REPL: %s\n",
                error_msg ? error_msg : "unknown");
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+I: Inspect annotations at cursor
    if (key == SDLK_I) {
      if (ctx->app.focused_pane) {
        Editor *editor = pane_get_editor(ctx->app.focused_pane);
        Buffer *buffer = editor_get_buffer(editor);
        if (editor && buffer) {
          size_t cursor_pos = editor_get_cursor_pos(editor);
          lcl_host_set_buffer(ctx->app.lcl, buffer);

          char cmd[64];
          snprintf(cmd, sizeof(cmd), "weft::inspect::at %zu", cursor_pos);

          const char *error_msg = NULL;
          if (!lcl_host_eval(ctx->app.lcl, cmd, &error_msg)) {
            fprintf(stderr, "Failed to inspect: %s\n",
                    error_msg ? error_msg : "unknown");
          }
        }
      }
      return SDL_APP_CONTINUE;
    }

    // Ctrl+Enter: Execute selection or line (keyboard middle-click)
    if (key == SDLK_RETURN) {
      if (ctx->app.focused_pane) {
        Editor *editor = pane_get_editor(ctx->app.focused_pane);
        Buffer *buffer = editor_get_buffer(editor);
        if (editor && buffer) {
          char *text_to_execute = NULL;
          size_t text_len = 0;
          size_t insert_pos = 0; // Where to insert result in REPL mode

          if (editor_has_selection(editor)) {
            // Use selected text
            size_t sel_start, sel_end;
            editor_get_selection(editor, &sel_start, &sel_end);
            text_len = sel_end - sel_start;
            insert_pos = sel_end;
            text_to_execute = malloc(text_len + 1);
            if (text_to_execute) {
              document_get_text(buffer->document, sel_start, text_to_execute,
                                text_len);
              text_to_execute[text_len] = '\0';
            }
          } else {
            // Use current line
            size_t cursor_pos = editor_get_cursor_pos(editor);
            size_t line_num =
                document_pos_to_line(buffer->document, cursor_pos);
            size_t line_start = document_line_start(buffer->document, line_num);
            size_t line_end = document_line_end(buffer->document, line_num);
            insert_pos = line_end;
            text_len = line_end - line_start;
            text_to_execute = malloc(text_len + 1);
            if (text_to_execute) {
              document_get_text(buffer->document, line_start, text_to_execute,
                                text_len);
              text_to_execute[text_len] = '\0';
              // Trim trailing whitespace/newline
              while (text_len > 0 && (text_to_execute[text_len - 1] == '\n' ||
                                      text_to_execute[text_len - 1] == '\r' ||
                                      text_to_execute[text_len - 1] == ' ' ||
                                      text_to_execute[text_len - 1] == '\t')) {
                text_to_execute[--text_len] = '\0';
              }
            }
          }

          if (text_to_execute && text_len > 0) {
            // Check if this is a REPL buffer
            bool is_repl =
                buffer->file_path && strcmp(buffer->file_path, "*lcl*") == 0;

            if (is_repl) {
              // REPL mode: evaluate as Lcl directly and insert result
              const char *error_msg = NULL;
              char *result = lcl_host_eval_to_string(
                  ctx->app.lcl, text_to_execute, &error_msg);
              bool success = (result != NULL);

              // Format the output
              char *output = NULL;
              if (success) {
                // Success: show "=> result"
                size_t output_len =
                    strlen(result) + 10; // "\n;; => " + result + "\n"
                output = malloc(output_len + 1);
                if (output) {
                  snprintf(output, output_len + 1, "\n;; => %s\n", result);
                }
                free(result);
              } else {
                // Error: show ";; ERROR: message"
                const char *err = error_msg ? error_msg : "unknown error";
                size_t output_len = strlen(err) + 16;
                output = malloc(output_len + 1);
                if (output) {
                  snprintf(output, output_len + 1, "\n;; ERROR: %s\n", err);
                }
              }

              if (output) {
                // Insert result after the evaluated line
                size_t out_len = strlen(output);
                document_insert(buffer->document, insert_pos, output, out_len);

                // Re-apply tree-sitter highlighting to the whole buffer
                // Try to find highlights.scm
                const char *hl_paths[] = {
                    "/home/brennan/dev/weft/submodules/tree-sitter-lcl/queries/highlights.scm",
                    "../submodules/tree-sitter-lcl/queries/highlights.scm",
                    NULL};
                const char *hl_path = NULL;
                for (const char **p = hl_paths; *p; p++) {
                  FILE *f = fopen(*p, "r");
                  if (f) {
                    fclose(f);
                    hl_path = *p;
                    break;
                  }
                }
                if (hl_path) {
                  TSHighlighter *hl = ts_hl_get_lcl(hl_path);
                  if (hl) {
                    ts_hl_apply_to_buffer(hl, buffer, hl_path);
                  }
                }

                // Add styling annotation for the result (overrides tree-sitter
                // for this range)
                const char *keys[] = {"style:fg"};
                const char *vals[] = {success ? "#88CC88" : "#CC8888"};
                annot_store_add(buffer->annots, insert_pos,
                                insert_pos + out_len, "style", keys, vals, 1);

                // Move cursor to end of inserted result
                editor->cursor_pos = insert_pos + out_len;
                editor->selection_anchor = SIZE_MAX;

                free(output);
              }
            } else {
              // Normal mode: create a virtual middle-click event
              InteractEvent *ie = interact_event_new(
                  INTERACT_MIDDLE_CLICK, editor_get_cursor_pos(editor), 0,
                  0, // screen coords not needed
                  event->key.mod);
              if (ie) {
                // Set the text as selected_text (takes priority in acme
                // handler)
                ie->has_selection = true;
                ie->selected_text = text_to_execute;
                text_to_execute = NULL; // ownership transferred

                // Dispatch through interaction system
                interaction_dispatch(ctx->app.lcl, editor, ie,
                                     ctx->app.focused_pane->layer_stack);
                interact_event_free(ie);
              }
            }
          }
          free(text_to_execute);
        }
      }
      return SDL_APP_CONTINUE;
    }
  }

  // Convert SDL events to InputEvents and dispatch
  InputEvent input_event = {.type = INPUT_NONE};

  switch (event->type) {
  case SDL_EVENT_KEY_DOWN:
    input_event.type = INPUT_KEY_DOWN;
    input_event.key.key = event->key.key;
    input_event.key.mod = event->key.mod;
    break;

  case SDL_EVENT_KEY_UP:
    input_event.type = INPUT_KEY_UP;
    input_event.key.key = event->key.key;
    input_event.key.mod = event->key.mod;
    break;

  case SDL_EVENT_TEXT_INPUT:
    input_event.type = INPUT_TEXT;
    strncpy(input_event.text.text, event->text.text,
            sizeof(input_event.text.text) - 1);
    input_event.text.text[sizeof(input_event.text.text) - 1] = '\0';
    break;

  case SDL_EVENT_MOUSE_BUTTON_DOWN: {
    int x = (int)event->button.x;
    int y = (int)event->button.y;
    int button = event->button.button;

    // First check if clicking on a divider (left button only)
    if (button == 1) {
      LayoutNode *divider = layout_divider_at_point(ctx->app.layout_root, x, y);
      if (divider) {
        ctx->app.dragging_divider = true;
        ctx->app.drag_split = divider;
        ctx->app.drag_start_pos = (divider->type == LAYOUT_SPLIT_H) ? x : y;
        break;
      }
    }

    // Check for pane click
    Pane *target = layout_pane_at_point(ctx->app.layout_root, x, y);
    if (target) {
      ctx->app.focused_pane = target;

      // Check for tag bar click (left button only)
      if (button == 1) {
        TagCommand cmd = pane_tag_bar_hit_test(target, x, y);
        if (cmd != TAG_CMD_NONE) {
          handle_tag_command(ctx, target, cmd);
          break;
        }

        // Check if click is in tag bar area (but not on a command)
        if (pane_point_in_tag_bar(target, x, y)) {
          // Just focus the pane, don't pass to editor
          break;
        }
      }

      // Get editor and translate coordinates
      Editor *editor = pane_get_editor(target);
      int local_x, local_y;
      pane_window_to_local(target, x, y, &local_x, &local_y);

      // For middle-click and right-click, try interaction dispatch
      if (button == 2 || button == 3) {
        size_t doc_pos = editor_screen_to_pos(editor, local_x, local_y);
        InteractType itype = button_to_interact_type(button);

        InteractEvent *ie =
            interact_event_new(itype, doc_pos, x, y, SDL_GetModState());
        if (ie) {
          interact_event_populate_context(ie, editor);

          InteractResult result = interaction_dispatch(ctx->app.lcl, editor, ie,
                                                       target->layer_stack);
          interact_event_free(ie);

          if (result == INTERACT_HANDLED) {
            // Recompute layout bounds in case new panes were added
            layout_compute_bounds(ctx->app.layout_root, get_window_rect(ctx));
            break;
          }
        }
        // If not handled, fall through to default behavior
      }

      // Left click: standard editor handling
      if (button == 1) {
        input_event.type = INPUT_MOUSE_DOWN;
        input_event.mouse.x = local_x;
        input_event.mouse.y = local_y;
        input_event.mouse.button = button;
        input_event.mouse.mod = SDL_GetModState();
      }
    }
    break;
  }

  case SDL_EVENT_MOUSE_BUTTON_UP: {
    int x = (int)event->button.x;
    int y = (int)event->button.y;

    // Stop divider dragging
    if (ctx->app.dragging_divider) {
      ctx->app.dragging_divider = false;
      ctx->app.drag_split = NULL;
      break;
    }

    // Translate to focused pane coordinates
    if (ctx->app.focused_pane) {
      int local_x, local_y;
      pane_window_to_local(ctx->app.focused_pane, x, y, &local_x, &local_y);

      input_event.type = INPUT_MOUSE_UP;
      input_event.mouse.x = local_x;
      input_event.mouse.y = local_y;
      input_event.mouse.button = event->button.button;
      input_event.mouse.mod = SDL_GetModState();
    }
    break;
  }

  case SDL_EVENT_MOUSE_MOTION: {
    int x = (int)event->motion.x;
    int y = (int)event->motion.y;

    // Handle divider dragging
    if (ctx->app.dragging_divider && ctx->app.drag_split) {
      int current_pos = (ctx->app.drag_split->type == LAYOUT_SPLIT_H) ? x : y;
      int delta = current_pos - ctx->app.drag_start_pos;
      if (delta != 0) {
        layout_adjust_split(ctx->app.drag_split, delta);
        layout_compute_bounds(ctx->app.layout_root, get_window_rect(ctx));
        ctx->app.drag_start_pos = current_pos;
      }
      break;
    }

    // Translate to focused pane coordinates (for drag operations)
    if (ctx->app.focused_pane) {
      int local_x, local_y;
      pane_window_to_local(ctx->app.focused_pane, x, y, &local_x, &local_y);

      input_event.type = INPUT_MOUSE_MOVE;
      input_event.mouse.x = local_x;
      input_event.mouse.y = local_y;
      input_event.mouse.button = 0;
      input_event.mouse.mod = SDL_GetModState();
    }
    break;
  }

  case SDL_EVENT_MOUSE_WHEEL:
    input_event.type = INPUT_SCROLL;
    input_event.scroll.dx = event->wheel.x;
    input_event.scroll.dy = event->wheel.y;
    break;

  default: break;
  }

  // Check if there's a pending transform prompt that needs to be shown
  if (ctx->app.focused_pane && ctx->app.lcl &&
      lcl_host_has_transform_prompt(ctx->app.lcl) &&
      !pane_prompt_is_active(ctx->app.focused_pane)) {
    const char *message = NULL;
    const char *default_value = NULL;
    if (lcl_host_get_transform_prompt(ctx->app.lcl, &message, &default_value)) {
      pane_prompt_show(ctx->app.focused_pane, message, default_value);
    }
  }

  // If search bar is active, route events there first
  if (input_event.type != INPUT_NONE && ctx->app.focused_pane &&
      pane_search_is_active(ctx->app.focused_pane)) {

    if (input_event.type == INPUT_KEY_DOWN) {
      if (pane_search_handle_key(ctx->app.focused_pane, input_event.key.key,
                                 input_event.key.mod)) {
        return SDL_APP_CONTINUE;
      }
    } else if (input_event.type == INPUT_TEXT) {
      if (pane_search_handle_text(ctx->app.focused_pane,
                                  input_event.text.text)) {
        return SDL_APP_CONTINUE;
      }
    }
  }

  // If goto dialog is active, route events there first
  if (input_event.type != INPUT_NONE && ctx->app.focused_pane &&
      pane_goto_is_active(ctx->app.focused_pane)) {

    if (input_event.type == INPUT_KEY_DOWN) {
      if (pane_goto_handle_key(ctx->app.focused_pane, input_event.key.key,
                               input_event.key.mod)) {
        return SDL_APP_CONTINUE;
      }
    } else if (input_event.type == INPUT_TEXT) {
      if (pane_goto_handle_text(ctx->app.focused_pane, input_event.text.text)) {
        return SDL_APP_CONTINUE;
      }
    }
  }

  // If transform prompt is active, route events there first
  if (input_event.type != INPUT_NONE && ctx->app.focused_pane &&
      pane_prompt_is_active(ctx->app.focused_pane)) {

    if (input_event.type == INPUT_KEY_DOWN) {
      SDL_Keycode key = input_event.key.key;

      // Handle Enter key specially - submit the prompt
      if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        const char *input = pane_prompt_get_input(ctx->app.focused_pane);
        if (input && ctx->app.lcl) {
          // Submit prompt - this shows diff preview if a patch is returned
          lcl_host_submit_transform_prompt(ctx->app.lcl, input);
        }
        pane_prompt_hide(ctx->app.focused_pane);
        return SDL_APP_CONTINUE;
      }

      // Handle Escape - cancel the prompt
      if (key == SDLK_ESCAPE) {
        lcl_host_cancel_transform_prompt(ctx->app.lcl);
        pane_prompt_hide(ctx->app.focused_pane);
        return SDL_APP_CONTINUE;
      }

      if (pane_prompt_handle_key(ctx->app.focused_pane, key,
                                 input_event.key.mod)) {
        return SDL_APP_CONTINUE;
      }
    } else if (input_event.type == INPUT_TEXT) {
      if (pane_prompt_handle_text(ctx->app.focused_pane,
                                  input_event.text.text)) {
        return SDL_APP_CONTINUE;
      }
    }
  }

  // Dispatch to focused pane's editor
  if (input_event.type != INPUT_NONE && ctx->app.focused_pane) {
    Editor *editor = pane_get_editor(ctx->app.focused_pane);
    if (editor) {
      editor_handle_event(editor, &input_event);
    }
  }

  return SDL_APP_CONTINUE;
}

// Render context for layout traversal
typedef struct {
  Renderer *renderer;
  Pane *focused_pane;
  uint64_t time;
  Buffer *repl_target; // Buffer attached to REPL (for visual indicator)
} RenderContext;

// Context for updating panes
typedef struct {
  uint64_t time;
  Pane *focused_pane;
} UpdateContext;

// Callback to update each pane's editor (only blink cursor on focused pane)
static void update_pane_editor(Pane *pane, void *data) {
  UpdateContext *uc = (UpdateContext *)data;
  if (pane == uc->focused_pane) {
    Editor *editor = pane_get_editor(pane);
    if (editor) {
      editor_update(editor, uc->time);
    }
  }
}

// Callback to render each pane
static void render_pane_callback(Pane *pane, void *data) {
  RenderContext *rc = (RenderContext *)data;
  bool focused = (pane == rc->focused_pane);
  pane_render(pane, rc->renderer, focused);

  // Draw border if this pane's buffer is the REPL target
  if (rc->repl_target && pane_get_buffer(pane) == rc->repl_target) {
    SDL_Rect bounds = pane_get_bounds(pane);
    SDL_Color border_color = {.r = 200, .g = 80, .b = 80, .a = 255}; // Reddish
    int thickness = 2;

    // Top edge
    renderer_draw_rect(rc->renderer, bounds.x, bounds.y, bounds.w, thickness,
                       border_color);
    // Bottom edge
    renderer_draw_rect(rc->renderer, bounds.x, bounds.y + bounds.h - thickness,
                       bounds.w, thickness, border_color);
    // Left edge
    renderer_draw_rect(rc->renderer, bounds.x, bounds.y, thickness, bounds.h,
                       border_color);
    // Right edge
    renderer_draw_rect(rc->renderer, bounds.x + bounds.w - thickness, bounds.y,
                       thickness, bounds.h, border_color);
  }
}

// Draw dividers between panes
static void draw_dividers(LayoutNode *node, Renderer *r) {
  if (!node || node->type == LAYOUT_LEAF) {
    return;
  }

  // Draw the divider for this split
  SDL_Rect first_bounds = pane_get_bounds(layout_first_pane(node->split.first));
  SDL_Color divider_color = {.r = 60, .g = 60, .b = 60, .a = 255};

  if (node->type == LAYOUT_SPLIT_H) {
    // Vertical divider
    int x = first_bounds.x + first_bounds.w;
    int y = first_bounds.y;
    int h = first_bounds.h;
    renderer_draw_rect(r, x, y, DIVIDER_SIZE, h, divider_color);
  } else {
    // Horizontal divider
    int x = first_bounds.x;
    int y = first_bounds.y + first_bounds.h;
    int w = first_bounds.w;
    renderer_draw_rect(r, x, y, w, DIVIDER_SIZE, divider_color);
  }

  // Recurse
  draw_dividers(node->split.first, r);
  draw_dividers(node->split.second, r);
}

// Debounce delay for layer refresh (ms)
#define LAYER_REFRESH_DEBOUNCE_MS 300

SDL_AppResult SDL_AppIterate(void *app_state) {
  AppContext *ctx = (AppContext *)app_state;

  if (!ctx->app.running) {
    return SDL_APP_SUCCESS;
  }

  uint64_t now = SDL_GetTicks();

  // Check if layers need refresh (debounced)
  if (lcl_host_has_dirty_layers(ctx->app.lcl)) {
    if (ctx->app.last_edit_time == 0) {
      // First time seeing dirty layers - record the time
      ctx->app.last_edit_time = now;
    } else if (now - ctx->app.last_edit_time >= LAYER_REFRESH_DEBOUNCE_MS) {
      // Enough time has passed - refresh dirty layers
      int refreshed = lcl_host_refresh_dirty_layers(ctx->app.lcl);
      if (refreshed > 0) {
        // printf("Refreshed %d layer(s)\n", refreshed);
      }
      ctx->app.last_edit_time = 0; // Reset for next edit cycle
    }
  } else {
    // No dirty layers - reset timer
    ctx->app.last_edit_time = 0;
  }

  // Update focused pane's editor (cursor blink)
  UpdateContext uc = {.time = now, .focused_pane = ctx->app.focused_pane};
  layout_for_each_pane(ctx->app.layout_root, update_pane_editor, &uc);

  // Clear with a dark background
  renderer_clear(ctx->app.renderer, 40, 40, 40);

  // Render all panes
  RenderContext rc = {.renderer = ctx->app.renderer,
                      .focused_pane = ctx->app.focused_pane,
                      .time = now,
                      .repl_target = lcl_host_get_repl_attached_buffer(ctx->app.lcl)};
  layout_for_each_pane(ctx->app.layout_root, render_pane_callback, &rc);

  // Draw dividers
  draw_dividers(ctx->app.layout_root, ctx->app.renderer);

  renderer_present(ctx->app.renderer);

  // Cap frame rate to ~60 FPS to avoid burning CPU
  SDL_Delay(16);

  return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *app_state, SDL_AppResult result) {
  (void)result;

  AppContext *ctx = (AppContext *)app_state;
  if (!ctx) {
    return;
  }

  // layout_free frees all panes (which own their editors)
  layout_free(ctx->app.layout_root);
  lcl_host_free(ctx->app.lcl);
  buffer_manager_free(ctx->app.buffers);
  renderer_free(ctx->app.renderer);
  platform_free(ctx->app.platform);
  free(ctx);
}
