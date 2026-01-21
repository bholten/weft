#include "layout.h"

#include <stdlib.h>

#include "pane.h"

LayoutNode *layout_leaf_new(Pane *pane) {
  if (!pane) {
    return NULL;
  }

  LayoutNode *node = calloc(1, sizeof(LayoutNode));
  if (!node) {
    return NULL;
  }

  node->type = LAYOUT_LEAF;
  node->parent = NULL;
  node->pane = pane;

  return node;
}

void layout_free(LayoutNode *node) {
  if (!node) {
    return;
  }

  if (node->type == LAYOUT_LEAF) {
    pane_free(node->pane);
  } else {
    layout_free(node->split.first);
    layout_free(node->split.second);
  }

  free(node);
}

// Internal: Create a split node
static LayoutNode *layout_split_new(LayoutNodeType type, LayoutNode *first,
                                    LayoutNode *second) {
  LayoutNode *node = calloc(1, sizeof(LayoutNode));
  if (!node) {
    return NULL;
  }

  node->type = type;
  node->parent = NULL;
  node->split.first = first;
  node->split.second = second;
  node->split.split_ratio = 0.5f;

  if (first) {
    first->parent = node;
  }
  if (second) {
    second->parent = node;
  }

  return node;
}

LayoutNode *layout_split_horizontal(LayoutNode *node, Pane *new_pane) {
  if (!node || !new_pane) {
    return NULL;
  }

  // Save original parent BEFORE layout_split_new modifies node->parent
  LayoutNode *original_parent = node->parent;

  LayoutNode *new_leaf = layout_leaf_new(new_pane);
  if (!new_leaf) {
    return NULL;
  }

  LayoutNode *split = layout_split_new(LAYOUT_SPLIT_H, node, new_leaf);
  if (!split) {
    layout_free(new_leaf);
    return NULL;
  }

  // Update parent linkage using saved original parent
  if (original_parent) {
    if (original_parent->split.first == node) {
      original_parent->split.first = split;
    } else {
      original_parent->split.second = split;
    }
    split->parent = original_parent;
  }

  return split;
}

LayoutNode *layout_split_vertical(LayoutNode *node, Pane *new_pane) {
  if (!node || !new_pane) {
    return NULL;
  }

  // Save original parent BEFORE layout_split_new modifies node->parent
  LayoutNode *original_parent = node->parent;

  LayoutNode *new_leaf = layout_leaf_new(new_pane);
  if (!new_leaf) {
    return NULL;
  }

  LayoutNode *split = layout_split_new(LAYOUT_SPLIT_V, node, new_leaf);
  if (!split) {
    layout_free(new_leaf);
    return NULL;
  }

  // Update parent linkage using saved original parent
  if (original_parent) {
    if (original_parent->split.first == node) {
      original_parent->split.first = split;
    } else {
      original_parent->split.second = split;
    }
    split->parent = original_parent;
  }

  return split;
}

LayoutNode *layout_close_pane(LayoutNode *root, Pane *pane) {
  if (!root || !pane) {
    return root;
  }

  // Find the leaf node containing this pane
  LayoutNode *leaf = layout_find_pane(root, pane);
  if (!leaf) {
    return root;
  }

  // If this is the root, return NULL (no panes remain)
  if (!leaf->parent) {
    layout_free(leaf);
    return NULL;
  }

  // Get parent and sibling
  LayoutNode *parent = leaf->parent;
  LayoutNode *sibling = (parent->split.first == leaf) ? parent->split.second
                                                      : parent->split.first;

  // Remove sibling from parent (don't free it, we're keeping it)
  sibling->parent = parent->parent;

  // Replace parent with sibling in grandparent
  if (parent->parent) {
    LayoutNode *grandparent = parent->parent;
    if (grandparent->split.first == parent) {
      grandparent->split.first = sibling;
    } else {
      grandparent->split.second = sibling;
    }
  }

  // Determine new root
  LayoutNode *new_root = (parent == root) ? sibling : root;

  // Free the leaf and parent (but not sibling or its children)
  pane_free(leaf->pane);
  free(leaf);
  // Clear parent's pointers so we don't double-free
  parent->split.first = NULL;
  parent->split.second = NULL;
  free(parent);

  return new_root;
}

void layout_compute_bounds(LayoutNode *node, SDL_Rect available) {
  if (!node) {
    return;
  }

  if (node->type == LAYOUT_LEAF) {
    pane_set_bounds(node->pane, available);
    return;
  }

  // Split node - divide available space
  if (node->type == LAYOUT_SPLIT_H) {
    // Horizontal split: first on left, second on right
    int first_width =
        (int)((float)available.w * node->split.split_ratio) - DIVIDER_SIZE / 2;
    int second_width = available.w - first_width - DIVIDER_SIZE;

    SDL_Rect first_rect = {
        .x = available.x, .y = available.y, .w = first_width, .h = available.h};
    SDL_Rect second_rect = {.x = available.x + first_width + DIVIDER_SIZE,
                            .y = available.y,
                            .w = second_width,
                            .h = available.h};

    layout_compute_bounds(node->split.first, first_rect);
    layout_compute_bounds(node->split.second, second_rect);
  } else {
    // Vertical split: first on top, second on bottom
    int first_height =
        (int)((float)available.h * node->split.split_ratio) - DIVIDER_SIZE / 2;
    int second_height = available.h - first_height - DIVIDER_SIZE;

    SDL_Rect first_rect = {.x = available.x,
                           .y = available.y,
                           .w = available.w,
                           .h = first_height};
    SDL_Rect second_rect = {.x = available.x,
                            .y = available.y + first_height + DIVIDER_SIZE,
                            .w = available.w,
                            .h = second_height};

    layout_compute_bounds(node->split.first, first_rect);
    layout_compute_bounds(node->split.second, second_rect);
  }
}

Pane *layout_pane_at_point(LayoutNode *node, int x, int y) {
  if (!node) {
    return NULL;
  }

  if (node->type == LAYOUT_LEAF) {
    if (pane_contains_point(node->pane, x, y)) {
      return node->pane;
    }
    return NULL;
  }

  // Check both children
  Pane *result = layout_pane_at_point(node->split.first, x, y);
  if (result) {
    return result;
  }
  return layout_pane_at_point(node->split.second, x, y);
}

LayoutNode *layout_find_pane(LayoutNode *root, Pane *pane) {
  if (!root || !pane) {
    return NULL;
  }

  if (root->type == LAYOUT_LEAF) {
    return (root->pane == pane) ? root : NULL;
  }

  LayoutNode *result = layout_find_pane(root->split.first, pane);
  if (result) {
    return result;
  }
  return layout_find_pane(root->split.second, pane);
}

Pane *layout_find_pane_with_buffer(LayoutNode *root, Buffer *buffer) {
  if (!root || !buffer) {
    return NULL;
  }

  if (root->type == LAYOUT_LEAF) {
    if (root->pane && pane_get_buffer(root->pane) == buffer) {
      return root->pane;
    }
    return NULL;
  }

  Pane *result = layout_find_pane_with_buffer(root->split.first, buffer);
  if (result) {
    return result;
  }
  return layout_find_pane_with_buffer(root->split.second, buffer);
}

void layout_for_each_pane(LayoutNode *node, LayoutPaneCallback cb, void *data) {
  if (!node || !cb) {
    return;
  }

  if (node->type == LAYOUT_LEAF) {
    cb(node->pane, data);
    return;
  }

  layout_for_each_pane(node->split.first, cb, data);
  layout_for_each_pane(node->split.second, cb, data);
}

Pane *layout_first_pane(LayoutNode *node) {
  if (!node) {
    return NULL;
  }

  if (node->type == LAYOUT_LEAF) {
    return node->pane;
  }

  return layout_first_pane(node->split.first);
}

// Internal: Collect all panes in order
static void collect_panes(LayoutNode *node, Pane ***panes, size_t *count,
                          size_t *cap) {
  if (!node) {
    return;
  }

  if (node->type == LAYOUT_LEAF) {
    if (*count >= *cap) {
      size_t new_cap = (*cap == 0) ? 8 : *cap * 2;
      Pane **new_panes = realloc(*panes, new_cap * sizeof(Pane *));
      if (!new_panes) {
        return;
      }
      *panes = new_panes;
      *cap = new_cap;
    }
    (*panes)[(*count)++] = node->pane;
    return;
  }

  collect_panes(node->split.first, panes, count, cap);
  collect_panes(node->split.second, panes, count, cap);
}

Pane *layout_next_pane(LayoutNode *root, Pane *current) {
  if (!root) {
    return NULL;
  }

  Pane **panes = NULL;
  size_t count = 0;
  size_t cap = 0;

  collect_panes(root, &panes, &count, &cap);

  if (count == 0) {
    free(panes);
    return NULL;
  }

  // Find current pane and return next
  Pane *result = panes[0]; // Default to first
  for (size_t i = 0; i < count; i++) {
    if (panes[i] == current) {
      result = panes[(i + 1) % count];
      break;
    }
  }

  free(panes);
  return result;
}

Pane *layout_prev_pane(LayoutNode *root, Pane *current) {
  if (!root) {
    return NULL;
  }

  Pane **panes = NULL;
  size_t count = 0;
  size_t cap = 0;

  collect_panes(root, &panes, &count, &cap);

  if (count == 0) {
    free(panes);
    return NULL;
  }

  // Find current pane and return previous
  Pane *result = panes[count - 1]; // Default to last
  for (size_t i = 0; i < count; i++) {
    if (panes[i] == current) {
      result = panes[(i + count - 1) % count];
      break;
    }
  }

  free(panes);
  return result;
}

LayoutNode *layout_divider_at_point(LayoutNode *node, int x, int y) {
  if (!node || node->type == LAYOUT_LEAF) {
    return NULL;
  }

  // Get bounds of first child to determine divider position
  SDL_Rect first_bounds = pane_get_bounds(layout_first_pane(node->split.first));

  if (node->type == LAYOUT_SPLIT_H) {
    // Vertical divider between left and right
    int divider_x = first_bounds.x + first_bounds.w;
    if (x >= divider_x && x < divider_x + DIVIDER_SIZE) {
      // Check y is within the split's range
      SDL_Rect second_bounds =
          pane_get_bounds(layout_first_pane(node->split.second));
      int min_y = first_bounds.y;
      int max_y = first_bounds.y + first_bounds.h;
      if (second_bounds.y < min_y) {
        min_y = second_bounds.y;
      }
      if (second_bounds.y + second_bounds.h > max_y) {
        max_y = second_bounds.y + second_bounds.h;
      }
      if (y >= min_y && y < max_y) {
        return node;
      }
    }
  } else {
    // Horizontal divider between top and bottom
    int divider_y = first_bounds.y + first_bounds.h;
    if (y >= divider_y && y < divider_y + DIVIDER_SIZE) {
      // Check x is within the split's range
      SDL_Rect second_bounds =
          pane_get_bounds(layout_first_pane(node->split.second));
      int min_x = first_bounds.x;
      int max_x = first_bounds.x + first_bounds.w;
      if (second_bounds.x < min_x) {
        min_x = second_bounds.x;
      }
      if (second_bounds.x + second_bounds.w > max_x) {
        max_x = second_bounds.x + second_bounds.w;
      }
      if (x >= min_x && x < max_x) {
        return node;
      }
    }
  }

  // Check children
  LayoutNode *result = layout_divider_at_point(node->split.first, x, y);
  if (result) {
    return result;
  }
  return layout_divider_at_point(node->split.second, x, y);
}

void layout_adjust_split(LayoutNode *split_node, int delta) {
  if (!split_node || split_node->type == LAYOUT_LEAF) {
    return;
  }

  // Get total size in the split direction
  SDL_Rect first_bounds =
      pane_get_bounds(layout_first_pane(split_node->split.first));
  int total_size;

  if (split_node->type == LAYOUT_SPLIT_H) {
    SDL_Rect second_bounds =
        pane_get_bounds(layout_first_pane(split_node->split.second));
    total_size = first_bounds.w + DIVIDER_SIZE + second_bounds.w;
  } else {
    SDL_Rect second_bounds =
        pane_get_bounds(layout_first_pane(split_node->split.second));
    total_size = first_bounds.h + DIVIDER_SIZE + second_bounds.h;
  }

  if (total_size <= 0) {
    return;
  }

  // Adjust ratio
  float delta_ratio = (float)delta / (float)total_size;
  split_node->split.split_ratio += delta_ratio;

  // Clamp ratio
  if (split_node->split.split_ratio < 0.1f) {
    split_node->split.split_ratio = 0.1f;
  }
  if (split_node->split.split_ratio > 0.9f) {
    split_node->split.split_ratio = 0.9f;
  }
}
