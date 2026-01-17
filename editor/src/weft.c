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

    // Get renderer metrics for pane creation
    int char_width = renderer_get_char_width(ctx->app.renderer);
    int line_height = renderer_get_line_height(ctx->app.renderer);

    // Create initial pane with the buffer
    Pane *initial_pane = pane_new(initial_buffer, char_width, line_height);
    if (!initial_pane) {
        buffer_manager_free(ctx->app.buffers);
        renderer_free(ctx->app.renderer);
        platform_free(ctx->app.platform);
        free(ctx);
        return SDL_APP_FAILURE;
    }

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

            if (event->button.button == 1) {  // Left click
                // First check if clicking on a divider
                LayoutNode *divider = layout_divider_at_point(ctx->app.layout_root, x, y);
                if (divider) {
                    ctx->app.dragging_divider = true;
                    ctx->app.drag_split = divider;
                    ctx->app.drag_start_pos = (divider->type == LAYOUT_SPLIT_H) ? x : y;
                    break;
                }

                // Check for pane click
                Pane *target = layout_pane_at_point(ctx->app.layout_root, x, y);
                if (target) {
                    ctx->app.focused_pane = target;

                    // Check for tag bar click
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

                    // Translate coordinates to pane-local
                    int local_x, local_y;
                    pane_window_to_local(target, x, y, &local_x, &local_y);

                    input_event.type = INPUT_MOUSE_DOWN;
                    input_event.mouse.x = local_x;
                    input_event.mouse.y = local_y;
                    input_event.mouse.button = event->button.button;
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

SDL_AppResult SDL_AppIterate(void *app_state) {
    AppContext *ctx = (AppContext *)app_state;

    if (!ctx->app.running) {
        return SDL_APP_SUCCESS;
    }

    uint64_t now = SDL_GetTicks();

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
    buffer_manager_free(ctx->app.buffers);
    renderer_free(ctx->app.renderer);
    platform_free(ctx->app.platform);
    free(ctx);
}
