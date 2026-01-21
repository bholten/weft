#ifndef WEFT_LAYOUT_H
#define WEFT_LAYOUT_H

#include <stdbool.h>

#include <SDL3/SDL.h>

#include "pane.h"

// Layout node types
typedef enum {
  LAYOUT_LEAF,    // Contains a Pane
  LAYOUT_SPLIT_H, // Horizontal split (left | right)
  LAYOUT_SPLIT_V  // Vertical split (top / bottom)
} LayoutNodeType;

// Layout tree node
typedef struct LayoutNode {
  LayoutNodeType type;
  struct LayoutNode *parent;
  union {
    Pane *pane; // For LAYOUT_LEAF
    struct {
      struct LayoutNode *first;  // Left or Top
      struct LayoutNode *second; // Right or Bottom
      float split_ratio;         // 0.0-1.0
    } split;
  };
} LayoutNode;

// Callback type for layout traversal
typedef void (*LayoutPaneCallback)(Pane *pane, void *data);

// Lifecycle
LayoutNode *layout_leaf_new(Pane *pane);
void layout_free(LayoutNode *node);

// Split operations - return the new split node (becomes parent of original)
LayoutNode *layout_split_horizontal(LayoutNode *node, Pane *new_pane);
LayoutNode *layout_split_vertical(LayoutNode *node, Pane *new_pane);

// Close a pane - returns new root if root changed, NULL if no panes remain
LayoutNode *layout_close_pane(LayoutNode *root, Pane *pane);

// Bounds computation
void layout_compute_bounds(LayoutNode *node, SDL_Rect available);

// Hit testing
Pane *layout_pane_at_point(LayoutNode *node, int x, int y);

// Find the node containing a pane
LayoutNode *layout_find_pane(LayoutNode *root, Pane *pane);

// Find a pane that contains a specific buffer
Pane *layout_find_pane_with_buffer(LayoutNode *root, Buffer *buffer);

// Traversal
void layout_for_each_pane(LayoutNode *node, LayoutPaneCallback cb, void *data);

// Get first/next/prev pane for focus cycling
Pane *layout_first_pane(LayoutNode *node);
Pane *layout_next_pane(LayoutNode *root, Pane *current);
Pane *layout_prev_pane(LayoutNode *root, Pane *current);

// Divider hit testing (returns the split node if hit, NULL otherwise)
// 4px divider zones between panes
#define DIVIDER_SIZE 4
LayoutNode *layout_divider_at_point(LayoutNode *node, int x, int y);

// Adjust split ratio based on drag
void layout_adjust_split(LayoutNode *split_node, int delta);

#endif // WEFT_LAYOUT_H
