#include "interaction.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "editor.h"
#include "buffer.h"
#include "document.h"
#include "annot_store.h"
#include "lcl_host.h"

// === Helper: Check if character is a word character ===
static bool is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// === Helper: Extract word at a position ===
static char *extract_word_at_pos(Document *doc, size_t pos) {
    size_t doc_len = document_length(doc);
    if (pos >= doc_len) {
        return NULL;
    }

    // Check if we're on a word character
    char c;
    document_get_text(doc, pos, &c, 1);
    if (!is_word_char(c)) {
        return NULL;
    }

    // Find word start
    size_t start = pos;
    while (start > 0) {
        document_get_text(doc, start - 1, &c, 1);
        if (!is_word_char(c)) {
            break;
        }
        start--;
    }

    // Find word end
    size_t end = pos;
    while (end < doc_len) {
        document_get_text(doc, end, &c, 1);
        if (!is_word_char(c)) {
            break;
        }
        end++;
    }

    size_t len = end - start;
    if (len == 0) {
        return NULL;
    }

    char *word = malloc(len + 1);
    if (!word) {
        return NULL;
    }

    document_get_text(doc, start, word, len);
    word[len] = '\0';
    return word;
}

// === InteractEvent lifecycle ===

InteractEvent *interact_event_new(InteractType type, size_t doc_pos,
                                   int screen_x, int screen_y,
                                   SDL_Keymod modifiers) {
    InteractEvent *ie = calloc(1, sizeof(InteractEvent));
    if (!ie) {
        return NULL;
    }

    ie->type = type;
    ie->doc_pos = doc_pos;
    ie->screen_x = screen_x;
    ie->screen_y = screen_y;
    ie->modifiers = modifiers;
    ie->handled = false;

    return ie;
}

void interact_event_populate_context(InteractEvent *ie, Editor *editor) {
    if (!ie || !editor) {
        return;
    }

    Buffer *buffer = editor_get_buffer(editor);
    if (!buffer) {
        return;
    }

    // Get selection info
    ie->has_selection = editor_has_selection(editor);
    if (ie->has_selection) {
        editor_get_selection(editor, &ie->sel_start, &ie->sel_end);

        // Extract selected text
        size_t sel_len = ie->sel_end - ie->sel_start;
        ie->selected_text = malloc(sel_len + 1);
        if (ie->selected_text) {
            document_get_text(buffer->document, ie->sel_start, ie->selected_text, sel_len);
            ie->selected_text[sel_len] = '\0';
        }
    }

    // Extract word at position
    ie->word_at_pos = extract_word_at_pos(buffer->document, ie->doc_pos);
}

void interact_event_free(InteractEvent *ie) {
    if (!ie) {
        return;
    }
    free(ie->selected_text);
    free(ie->word_at_pos);
    free(ie);
}

// === Dispatch ===

InteractResult interaction_dispatch(LclHost *lcl, Editor *editor, InteractEvent *ie) {
    if (!lcl || !editor || !ie) {
        return INTERACT_UNHANDLED;
    }

    Buffer *buffer = editor_get_buffer(editor);
    if (!buffer || !buffer->annots) {
        return INTERACT_UNHANDLED;
    }

    // First, check for annotation handlers at this position
    // Look for annotations with action:on_* metadata
    const char *action_key = NULL;
    switch (ie->type) {
        case INTERACT_LEFT_CLICK:
            action_key = "action:on_click";
            break;
        case INTERACT_MIDDLE_CLICK:
            action_key = "action:on_middle_click";
            break;
        case INTERACT_RIGHT_CLICK:
            action_key = "action:on_right_click";
            break;
        case INTERACT_HOVER:
            action_key = "action:on_hover";
            break;
    }

    // Query annotations at position in the "action" layer
    AnnotQuery query = {0};
    annot_store_at(buffer->annots, ie->doc_pos, "action", &query);

    // Check each annotation for matching action handler
    for (size_t i = 0; i < query.count; i++) {
        const AnnotRecord *rec = annot_store_get(buffer->annots, query.ids[i]);
        if (!rec) {
            continue;
        }

        // Look for the action handler in metadata
        const char *handler_script = NULL;
        for (size_t j = 0; j < rec->meta_count; j++) {
            if (strcmp(rec->keys[j], action_key) == 0) {
                handler_script = rec->values[j];
                break;
            }
        }

        if (handler_script) {
            // Set up context and run the handler
            lcl_host_set_current_event(lcl, ie, editor);
            const char *error_msg = NULL;
            bool ok = lcl_host_eval(lcl, handler_script, &error_msg);
            lcl_host_clear_current_event(lcl);

            if (!ok) {
                fprintf(stderr, "Interaction handler error: %s\n",
                        error_msg ? error_msg : "unknown");
            }

            if (ie->handled) {
                annot_query_free(&query);
                return INTERACT_HANDLED;
            }
        }
    }
    annot_query_free(&query);

    // Next, try global handlers registered with the Lcl host
    if (lcl_host_dispatch_global_handlers(lcl, ie, editor)) {
        return INTERACT_HANDLED;
    }

    return INTERACT_UNHANDLED;
}

// === Helper: Convert SDL button to InteractType ===

InteractType button_to_interact_type(int button) {
    switch (button) {
        case 1:
            return INTERACT_LEFT_CLICK;
        case 2:
            return INTERACT_MIDDLE_CLICK;
        case 3:
            return INTERACT_RIGHT_CLICK;
        default:
            return INTERACT_LEFT_CLICK;
    }
}
