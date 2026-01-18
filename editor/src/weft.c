#define SDL_MAIN_USE_CALLBACKS

#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "weft.h"
#include "platform.h"
#include "renderer.h"
#include "input.h"
#include "document.h"
#include "annot_store.h"
#include "buffer.h"
#include "editor.h"
#include "pane.h"
#include "layout.h"
#include "lcl_host.h"
#include "interaction.h"

static void test_document(void) {
    printf("=== Document Tests ===\n");

    // Test 1: Create from string
    const char *text = "hello\nworld";
    Document *doc = document_from_string(text, strlen(text));
    printf("Test 1 - Create from string:\n");
    printf("  Length: %zu (expected 11)\n", document_length(doc));
    printf("  Lines: %zu (expected 2)\n", document_line_count(doc));

    // Test 2: Get text
    char buf[64];
    size_t got = document_get_text(doc, 0, buf, sizeof(buf));
    buf[got] = '\0';
    printf("Test 2 - Get text:\n");
    printf("  Content: \"%s\" (expected \"hello\\nworld\")\n", buf);

    // Test 3: Line operations
    printf("Test 3 - Line operations:\n");
    printf("  Line 0 start: %zu (expected 0)\n", document_line_start(doc, 0));
    printf("  Line 0 end: %zu (expected 5)\n", document_line_end(doc, 0));
    printf("  Line 0 length: %zu (expected 6)\n", document_line_length(doc, 0));
    printf("  Line 1 start: %zu (expected 6)\n", document_line_start(doc, 1));
    printf("  Line 1 end: %zu (expected 11)\n", document_line_end(doc, 1));
    printf("  Line 1 length: %zu (expected 5)\n", document_line_length(doc, 1));

    // Test 4: Position to line
    printf("Test 4 - Position to line:\n");
    printf("  Pos 0 -> line %zu (expected 0)\n", document_pos_to_line(doc, 0));
    printf("  Pos 5 -> line %zu (expected 0)\n", document_pos_to_line(doc, 5));
    printf("  Pos 6 -> line %zu (expected 1)\n", document_pos_to_line(doc, 6));
    printf("  Pos 10 -> line %zu (expected 1)\n", document_pos_to_line(doc, 10));

    // Test 5: Insert
    printf("Test 5 - Insert:\n");
    document_insert(doc, 5, "X", 1);
    got = document_get_text(doc, 0, buf, sizeof(buf));
    buf[got] = '\0';
    printf("  After insert 'X' at 5: \"%s\" (expected \"helloX\\nworld\")\n", buf);
    printf("  Length: %zu (expected 12)\n", document_length(doc));
    printf("  Lines: %zu (expected 2)\n", document_line_count(doc));
    printf("  Line 0 end: %zu (expected 6)\n", document_line_end(doc, 0));

    // Test 6: Insert newline
    document_insert(doc, 3, "\n", 1);
    got = document_get_text(doc, 0, buf, sizeof(buf));
    buf[got] = '\0';
    printf("Test 6 - Insert newline:\n");
    printf("  After insert '\\n' at 3: \"%s\"\n", buf);
    printf("  Lines: %zu (expected 3)\n", document_line_count(doc));

    // Test 7: Delete
    printf("Test 7 - Delete:\n");
    document_delete(doc, 0, 4);  // Delete "hel\n"
    got = document_get_text(doc, 0, buf, sizeof(buf));
    buf[got] = '\0';
    printf("  After delete 4 chars at 0: \"%s\" (expected \"loX\\nworld\")\n", buf);
    printf("  Length: %zu (expected 9)\n", document_length(doc));
    printf("  Lines: %zu (expected 2)\n", document_line_count(doc));

    document_free(doc);

    // Test 8: Empty document
    printf("Test 8 - Empty document:\n");
    doc = document_new();
    printf("  Length: %zu (expected 0)\n", document_length(doc));
    printf("  Lines: %zu (expected 1)\n", document_line_count(doc));
    document_insert(doc, 0, "test", 4);
    got = document_get_text(doc, 0, buf, sizeof(buf));
    buf[got] = '\0';
    printf("  After insert: \"%s\" (expected \"test\")\n", buf);
    document_free(doc);

    // Test 9: Insert at start and end
    printf("Test 9 - Insert at boundaries:\n");
    doc = document_from_string("mid", 3);
    document_insert(doc, 0, "start-", 6);
    document_insert(doc, document_length(doc), "-end", 4);
    got = document_get_text(doc, 0, buf, sizeof(buf));
    buf[got] = '\0';
    printf("  Content: \"%s\" (expected \"start-mid-end\")\n", buf);
    document_free(doc);

    printf("=== Tests Complete ===\n\n");
}

// Application context using our new modules
typedef struct {
    WeftApp app;
    InputState input_state;
} AppContext;

SDL_AppResult SDL_AppInit(void **app_state, int argc, char **argv) {
    test_document();

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

    // Load font
    if (!renderer_load_font(ctx->app.renderer,
            "/home/brennan/dev/weft/editor/fonts/Roboto_Mono/RobotoMono-VariableFont_wght.ttf", 14)) {
        renderer_free(ctx->app.renderer);
        platform_free(ctx->app.platform);
        free(ctx);
        return SDL_APP_FAILURE;
    }

    // Create buffer manager
    ctx->app.buffers = buffer_manager_new();
    if (!ctx->app.buffers) {
        renderer_free(ctx->app.renderer);
        platform_free(ctx->app.platform);
        free(ctx);
        return SDL_APP_FAILURE;
    }

    // Create initial buffer (load file or create empty)
    Buffer *initial_buffer = NULL;
    if (argc > 1) {
        initial_buffer = buffer_manager_open_file(ctx->app.buffers, argv[1]);
        if (!initial_buffer) {
            SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Failed to load file: %s", argv[1]);
        }
    }
    if (!initial_buffer) {
        initial_buffer = buffer_manager_create(ctx->app.buffers);
    }
    if (!initial_buffer) {
        buffer_manager_free(ctx->app.buffers);
        renderer_free(ctx->app.renderer);
        platform_free(ctx->app.platform);
        free(ctx);
        return SDL_APP_FAILURE;
    }

    // Add test content and annotations if buffer is empty (new buffer)
    if (document_length(initial_buffer->document) == 0) {
        const char *test_text = "Hello, Weft!\nThis is styled text with annotations.\nRed, green, and blue words here.\nUnderlined text too!";
        document_insert(initial_buffer->document, 0, test_text, strlen(test_text));

        // Add test annotations to demonstrate styling
        // "Weft" in cyan (bytes 7-11 of line 1)
        const char *keys1[] = { "style:fg" };
        const char *vals1[] = { "cyan" };
        annot_store_add(initial_buffer->annots, 7, 11, "style", keys1, vals1, 1);

        // "styled" in yellow with underline (line 2)
        const char *keys2[] = { "style:fg", "style:underline" };
        const char *vals2[] = { "yellow", "true" };
        annot_store_add(initial_buffer->annots, 21, 27, "style", keys2, vals2, 2);

        // "Red" in red (line 3)
        const char *keys3[] = { "style:fg" };
        const char *vals3[] = { "red" };
        annot_store_add(initial_buffer->annots, 60, 63, "style", keys3, vals3, 1);

        // "green" in green (line 3)
        const char *keys4[] = { "style:fg" };
        const char *vals4[] = { "green" };
        annot_store_add(initial_buffer->annots, 65, 70, "style", keys4, vals4, 1);

        // "blue" in blue (line 3)
        const char *keys5[] = { "style:fg" };
        const char *vals5[] = { "#0088FF" };
        annot_store_add(initial_buffer->annots, 76, 80, "style", keys5, vals5, 1);

        // "Underlined" with underline and orange bg (line 4)
        const char *keys6[] = { "style:underline", "style:bg" };
        const char *vals6[] = { "true", "#442200" };
        annot_store_add(initial_buffer->annots, 94, 104, "style", keys6, vals6, 2);

        printf("Added test annotations for styling demo\n");
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
                              &ctx->app.layout_root, ctx,
                              char_width, line_height,
                              win_w, win_h);

    // Load user configuration (~/.config/weft/init.lcl)
    if (!lcl_host_load_config(ctx->app.lcl)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_CUSTOM, "Failed to load config file");
    }

    // Apply file type associations to initial buffer (syntax highlighting, etc.)
    lcl_host_apply_filetypes(ctx->app.lcl, initial_buffer);

    // Register default Acme-like middle-click handler
    // Recognizes:
    //   /path/to/file -> open file
    //   weft::* -> evaluate as Lcl command
    //   anything else -> execute as shell command
    const char *acme_handler =
        "let sel [weft::interact::selection]\n"
        "var text \"\"\n"
        "if [> [len $sel] 0] {\n"
        "    set! text [List::index $sel 2]\n"
        "} else {\n"
        "    set! text [weft::interact::word-at-pos]\n"
        "}\n"
        "if [== $text \"\"] { return }\n"
        "puts (str \"acme: executing '\" $text \"'\")\n"
        "if [== [String::index $text 0] \"/\"] {\n"
        "    weft::buffer::open $text\n"
        "    weft::interact::handled\n"
        "    return\n"
        "}\n"
        "if [== [String::range $text 0 6] \"weft::\"] {\n"
        "    eval $text\n"
        "    weft::interact::handled\n"
        "    return\n"
        "}\n"
        "let result [weft::shell::exec $text]\n"
        "let stdout [List::index $result 3]\n"
        "let stderr [List::index $result 5]\n"
        "if [> [len $stdout] 0] {\n"
        "    weft::buffer::open-output (str \"+\" $text) $stdout\n"
        "} else {\n"
        "    if [> [len $stderr] 0] {\n"
        "        weft::buffer::open-output (str \"!\" $text) $stderr\n"
        "    }\n"
        "}\n"
        "weft::interact::handled\n";

    lcl_host_register_handler(ctx->app.lcl, "acme-middle-click",
                               HANDLER_MIDDLE_CLICK, acme_handler, 0);

    // Test Lcl evaluation
    const char *error_msg = NULL;
    printf("=== Lcl Integration Test ===\n");
    if (lcl_host_eval(ctx->app.lcl, "puts [weft::version]", &error_msg)) {
        printf("Lcl: weft::version evaluated successfully\n");
    } else {
        printf("Lcl error: %s\n", error_msg ? error_msg : "unknown");
    }

    // Test document bindings
    if (lcl_host_eval(ctx->app.lcl, "puts (str \"Doc length: \" [weft::doc::length])", &error_msg)) {
        printf("Lcl: doc::length evaluated successfully\n");
    } else {
        printf("Lcl error: %s\n", error_msg ? error_msg : "unknown");
    }

    if (lcl_host_eval(ctx->app.lcl, "puts (str \"Line count: \" [weft::doc::line-count])", &error_msg)) {
        printf("Lcl: doc::line-count evaluated successfully\n");
    } else {
        printf("Lcl error: %s\n", error_msg ? error_msg : "unknown");
    }

    // Test annotation bindings
    if (lcl_host_eval(ctx->app.lcl, "let id [weft::annot::add 0 5 \"test\" \"style:fg\" \"magenta\"]", &error_msg)) {
        printf("Lcl: annot::add evaluated successfully\n");
    } else {
        printf("Lcl error: %s\n", error_msg ? error_msg : "unknown");
    }

    if (lcl_host_eval(ctx->app.lcl, "puts (str \"Annots at 2: \" [weft::annot::at 2])", &error_msg)) {
        printf("Lcl: annot::at evaluated successfully\n");
    } else {
        printf("Lcl error: %s\n", error_msg ? error_msg : "unknown");
    }

    if (lcl_host_eval(ctx->app.lcl, "puts (str \"Annot info: \" [weft::annot::get $id])", &error_msg)) {
        printf("Lcl: annot::get evaluated successfully\n");
    } else {
        printf("Lcl error: %s\n", error_msg ? error_msg : "unknown");
    }

    // Test layer execution with a simple inline script
    const char *demo_layer =
        ";; Simple demo layer - highlight 'Hello' in green\n"
        "weft::annot::add 0 5 \"demo\" \"style:fg\" \"#00FF00\"\n";

    if (lcl_host_run_layer(ctx->app.lcl, "demo", demo_layer, &error_msg)) {
        printf("Lcl: demo layer executed successfully\n");
    } else {
        printf("Lcl layer error: %s\n", error_msg ? error_msg : "unknown");
    }
    printf("=== Lcl Test Complete ===\n\n");

    // Create initial pane with the buffer (uses char_width, line_height from earlier)
    Pane *initial_pane = pane_new(initial_buffer, char_width, line_height);
    if (!initial_pane) {
        buffer_manager_free(ctx->app.buffers);
        renderer_free(ctx->app.renderer);
        platform_free(ctx->app.platform);
        free(ctx);
        return SDL_APP_FAILURE;
    }

    // Apply indent settings based on file type
    lcl_host_apply_indent(ctx->app.lcl, pane_get_editor(initial_pane));

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
    SDL_Rect window_rect = { .x = 0, .y = 0, .w = 800, .h = 600 };
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

    return new_pane;
}

// Helper to get window bounds
static SDL_Rect get_window_rect(AppContext *ctx) {
    int w, h;
    SDL_GetWindowSize(platform_get_window(ctx->app.platform), &w, &h);
    return (SDL_Rect){ .x = 0, .y = 0, .w = w, .h = h };
}

// File dialog callback data
typedef struct {
    AppContext *ctx;
    Pane *target_pane;
    bool save_as;  // true for SaveAs, false for Open
} FileDialogData;

// Open file dialog callback
static void open_file_callback(void *userdata, const char * const *filelist, int filter) {
    (void)filter;
    FileDialogData *data = (FileDialogData *)userdata;

    if (filelist && filelist[0]) {
        Buffer *buf = pane_get_buffer(data->target_pane);
        if (buf) {
            buffer_load(buf, filelist[0]);
        }
    }

    free(data);
}

// Save file dialog callback
static void save_file_callback(void *userdata, const char * const *filelist, int filter) {
    (void)filter;
    FileDialogData *data = (FileDialogData *)userdata;

    if (filelist && filelist[0]) {
        Buffer *buf = pane_get_buffer(data->target_pane);
        if (buf) {
            buffer_save_as(buf, filelist[0]);
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
                SDL_ShowOpenFileDialog(
                    open_file_callback,
                    data,
                    platform_get_window(ctx->app.platform),
                    NULL,  // No filters
                    0,     // No filter count
                    NULL,  // Default location
                    false  // Single file
                );
            }
            break;
        }

        case TAG_CMD_SAVE: {
            Buffer *buf = pane_get_buffer(pane);
            if (buf) {
                if (buf->file_path) {
                    buffer_save(buf);
                } else {
                    // No path - fall through to SaveAs
                    FileDialogData *data = calloc(1, sizeof(FileDialogData));
                    if (data) {
                        data->ctx = ctx;
                        data->target_pane = pane;
                        data->save_as = true;
                        SDL_ShowSaveFileDialog(
                            save_file_callback,
                            data,
                            platform_get_window(ctx->app.platform),
                            NULL,
                            0,
                            NULL
                        );
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
                SDL_ShowSaveFileDialog(
                    save_file_callback,
                    data,
                    platform_get_window(ctx->app.platform),
                    NULL,
                    0,
                    NULL
                );
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

        default:
            break;
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
        SDL_Rect window_rect = { .x = 0, .y = 0, .w = event->window.data1, .h = event->window.data2 };
        layout_compute_bounds(ctx->app.layout_root, window_rect);
        // Update Lcl host with new window dimensions
        lcl_host_set_window_size(ctx->app.lcl, event->window.data1, event->window.data2);
    }

    // Handle app-level key commands (split/close/focus)
    if (event->type == SDL_EVENT_KEY_DOWN && (event->key.mod & SDL_KMOD_CTRL)) {
        SDL_Keycode key = event->key.key;

        // Ctrl+2: Split horizontally
        if (key == SDLK_2) {
            Pane *new_pane = create_new_pane(ctx);
            if (new_pane) {
                LayoutNode *current_leaf = layout_find_pane(ctx->app.layout_root, ctx->app.focused_pane);
                if (current_leaf) {
                    LayoutNode *new_split = layout_split_horizontal(current_leaf, new_pane);
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
                LayoutNode *current_leaf = layout_find_pane(ctx->app.layout_root, ctx->app.focused_pane);
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

        // Ctrl+W: Close current pane
        if (key == SDLK_W) {
            Pane *next = layout_next_pane(ctx->app.layout_root, ctx->app.focused_pane);
            if (next && next != ctx->app.focused_pane) {
                ctx->app.layout_root = layout_close_pane(ctx->app.layout_root, ctx->app.focused_pane);
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
            Pane *next = layout_next_pane(ctx->app.layout_root, ctx->app.focused_pane);
            if (next) {
                ctx->app.focused_pane = next;
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

                    if (editor_has_selection(editor)) {
                        // Use selected text
                        size_t sel_start, sel_end;
                        editor_get_selection(editor, &sel_start, &sel_end);
                        text_len = sel_end - sel_start;
                        text_to_execute = malloc(text_len + 1);
                        if (text_to_execute) {
                            document_get_text(buffer->document, sel_start, text_to_execute, text_len);
                            text_to_execute[text_len] = '\0';
                        }
                    } else {
                        // Use current line
                        size_t cursor_pos = editor_get_cursor_pos(editor);
                        size_t line_num = document_pos_to_line(buffer->document, cursor_pos);
                        size_t line_start = document_line_start(buffer->document, line_num);
                        size_t line_end = document_line_end(buffer->document, line_num);
                        text_len = line_end - line_start;
                        text_to_execute = malloc(text_len + 1);
                        if (text_to_execute) {
                            document_get_text(buffer->document, line_start, text_to_execute, text_len);
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
                        // Create a virtual middle-click event
                        InteractEvent *ie = interact_event_new(
                            INTERACT_MIDDLE_CLICK,
                            editor_get_cursor_pos(editor),
                            0, 0,  // screen coords not needed
                            event->key.mod
                        );
                        if (ie) {
                            // Set the text as selected_text (takes priority in acme handler)
                            ie->has_selection = true;
                            ie->selected_text = text_to_execute;
                            text_to_execute = NULL;  // ownership transferred

                            // Dispatch through interaction system
                            interaction_dispatch(ctx->app.lcl, editor, ie);
                            interact_event_free(ie);
                        }
                    }
                    free(text_to_execute);
                }
            }
            return SDL_APP_CONTINUE;
        }
    }

    // Convert SDL events to InputEvents and dispatch
    InputEvent input_event = { .type = INPUT_NONE };

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
            strncpy(input_event.text.text, event->text.text, sizeof(input_event.text.text) - 1);
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

                    InteractEvent *ie = interact_event_new(itype, doc_pos, x, y,
                                                           SDL_GetModState());
                    if (ie) {
                        interact_event_populate_context(ie, editor);

                        InteractResult result = interaction_dispatch(ctx->app.lcl, editor, ie);
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

        default:
            break;
    }

    // If search bar is active, route events there first
    if (input_event.type != INPUT_NONE && ctx->app.focused_pane &&
        pane_search_is_active(ctx->app.focused_pane)) {

        if (input_event.type == INPUT_KEY_DOWN) {
            if (pane_search_handle_key(ctx->app.focused_pane,
                                       input_event.key.key,
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
            if (pane_goto_handle_key(ctx->app.focused_pane,
                                     input_event.key.key,
                                     input_event.key.mod)) {
                return SDL_APP_CONTINUE;
            }
        } else if (input_event.type == INPUT_TEXT) {
            if (pane_goto_handle_text(ctx->app.focused_pane,
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
}

// Draw dividers between panes
static void draw_dividers(LayoutNode *node, Renderer *r) {
    if (!node || node->type == LAYOUT_LEAF) {
        return;
    }

    // Draw the divider for this split
    SDL_Rect first_bounds = pane_get_bounds(layout_first_pane(node->split.first));
    SDL_Color divider_color = { .r = 60, .g = 60, .b = 60, .a = 255 };

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
            ctx->app.last_edit_time = 0;  // Reset for next edit cycle
        }
    } else {
        // No dirty layers - reset timer
        ctx->app.last_edit_time = 0;
    }

    // Update focused pane's editor (cursor blink)
    UpdateContext uc = { .time = now, .focused_pane = ctx->app.focused_pane };
    layout_for_each_pane(ctx->app.layout_root, update_pane_editor, &uc);

    // Clear with a dark background
    renderer_clear(ctx->app.renderer, 40, 40, 40);

    // Render all panes
    RenderContext rc = {
        .renderer = ctx->app.renderer,
        .focused_pane = ctx->app.focused_pane,
        .time = now
    };
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
