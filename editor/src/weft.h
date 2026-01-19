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
typedef struct LclHost LclHost;

// Main application context
typedef struct {
  Platform *platform;
  Renderer *renderer;
  BufferManager *buffers;  // Manages all open documents
  LayoutNode *layout_root; // Binary split tree
  Pane *focused_pane;      // Currently focused pane
  LclHost *lcl;            // Lcl scripting host
  bool running;

  // Divider drag state
  bool dragging_divider;
  LayoutNode *drag_split; // The split node being adjusted
  int drag_start_pos;     // Mouse position at drag start

  // Layer auto-refresh state
  uint64_t last_edit_time;  // When last edit occurred (for debouncing)
  bool layers_need_refresh; // Flag indicating layers should be refreshed
} WeftApp;

#endif // WEFT_H
