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
#include "annotation.h"
#include "editor.h"

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
    (void)argc;
    (void)argv;

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

    // Create sample document
    const char *sample_text = "Hello, Weft!\nThis is a test document.\nLine 3 here.";
    ctx->app.document = document_from_string(sample_text, strlen(sample_text));
    if (!ctx->app.document) {
        renderer_free(ctx->app.renderer);
        platform_free(ctx->app.platform);
        free(ctx);
        return SDL_APP_FAILURE;
    }

    // Create annotation store (empty for now)
    ctx->app.annotations = annotation_new();
    if (!ctx->app.annotations) {
        document_free(ctx->app.document);
        renderer_free(ctx->app.renderer);
        platform_free(ctx->app.platform);
        free(ctx);
        return SDL_APP_FAILURE;
    }

    // Create editor
    ctx->app.editor = editor_new(ctx->app.document);
    if (!ctx->app.editor) {
        annotation_free(ctx->app.annotations);
        document_free(ctx->app.document);
        renderer_free(ctx->app.renderer);
        platform_free(ctx->app.platform);
        free(ctx);
        return SDL_APP_FAILURE;
    }

    // Set editor layout from renderer metrics
    int char_width = renderer_get_char_width(ctx->app.renderer);
    int line_height = renderer_get_line_height(ctx->app.renderer);
    editor_set_layout(ctx->app.editor, char_width, line_height, 10, 10);

    // Initialize input state
    input_state_init(&ctx->input_state);

    // Enable text input
    SDL_StartTextInput(platform_get_window(ctx->app.platform));

    ctx->app.running = true;
    *app_state = ctx;

    return SDL_APP_CONTINUE;
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
    }

    // Convert SDL events to InputEvents and dispatch to editor
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

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            input_event.type = INPUT_MOUSE_DOWN;
            input_event.mouse.x = (int)event->button.x;
            input_event.mouse.y = (int)event->button.y;
            input_event.mouse.button = event->button.button;
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            input_event.type = INPUT_MOUSE_UP;
            input_event.mouse.x = (int)event->button.x;
            input_event.mouse.y = (int)event->button.y;
            input_event.mouse.button = event->button.button;
            break;

        default:
            break;
    }

    if (input_event.type != INPUT_NONE) {
        editor_handle_event(ctx->app.editor, &input_event);
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *app_state) {
    AppContext *ctx = (AppContext *)app_state;

    if (!ctx->app.running) {
        return SDL_APP_SUCCESS;
    }

    uint64_t now = SDL_GetTicks();

    // Update editor (cursor blink)
    editor_update(ctx->app.editor, now);

    // Animated background (keep the fun demo effect)
    float time = (float)now / 1000.f;
    uint8_t red = (uint8_t)(((sinf(time) + 1.0f) / 2.0f) * 255.0f);
    uint8_t green = (uint8_t)(((sinf(time / 2.0f) + 1.0f) / 2.0f) * 255.0f);
    uint8_t blue = (uint8_t)(((sinf(time * 2.0f) + 1.0f) / 2.0f) * 255.0f);

    renderer_clear(ctx->app.renderer, red, green, blue);

    // Draw document content line by line
    SDL_Color text_color = { .r = 255, .g = 255, .b = 255, .a = 255 };
    int line_height = renderer_get_line_height(ctx->app.renderer);
    int y = 10;

    size_t line_count = document_line_count(ctx->app.document);
    char line_buf[256];

    for (size_t i = 0; i < line_count; i++) {
        size_t start = document_line_start(ctx->app.document, i);
        size_t end = document_line_end(ctx->app.document, i);
        size_t len = end - start;

        if (len > sizeof(line_buf) - 1) {
            len = sizeof(line_buf) - 1;
        }

        size_t got = document_get_text(ctx->app.document, start, line_buf, len);
        line_buf[got] = '\0';

        renderer_draw_text(ctx->app.renderer, line_buf, 10, y, text_color);
        y += line_height;
    }

    // Draw cursor
    if (editor_cursor_is_visible(ctx->app.editor)) {
        int cursor_x, cursor_y;
        editor_pos_to_screen(ctx->app.editor, editor_get_cursor_pos(ctx->app.editor), &cursor_x, &cursor_y);

        SDL_Color cursor_color = { .r = 255, .g = 255, .b = 255, .a = 255 };
        renderer_draw_rect(ctx->app.renderer, cursor_x, cursor_y, 2, line_height, cursor_color);
    }

    renderer_present(ctx->app.renderer);

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *app_state, SDL_AppResult result) {
    (void)result;

    AppContext *ctx = (AppContext *)app_state;
    if (!ctx) {
        return;
    }

    editor_free(ctx->app.editor);
    annotation_free(ctx->app.annotations);
    document_free(ctx->app.document);
    renderer_free(ctx->app.renderer);
    platform_free(ctx->app.platform);
    free(ctx);
}
