#ifndef WEFT_H
#define WEFT_H

#include <stdbool.h>

#include <SDL3/SDL.h>

// Forward declarations
typedef struct Platform Platform;
typedef struct Renderer Renderer;
typedef struct BufferManager BufferManager;
typedef struct Pane Pane;
typedef struct LayoutNode LayoutNode;

// Main application context
typedef struct {
    Platform *platform;
    Renderer *renderer;
    BufferManager *buffers;     // Manages all open documents
    LayoutNode *layout_root;    // Binary split tree
    Pane *focused_pane;         // Currently focused pane
    bool running;

    // Divider drag state
    bool dragging_divider;
    LayoutNode *drag_split;     // The split node being adjusted
    int drag_start_pos;         // Mouse position at drag start
} WeftApp;

#endif // WEFT_H
