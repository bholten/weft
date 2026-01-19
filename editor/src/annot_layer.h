#ifndef WEFT_ANNOT_LAYER_H
#define WEFT_ANNOT_LAYER_H

#include "layer.h"

// PersistentAnnotationLayer: bridges AnnotStore to the ViewLayer protocol
// This layer reads from the annotation store's "style" (and optionally other)
// layers and emits decorations, allowing the legacy annotation system to
// work within the new layer stack architecture.

// Create a PersistentAnnotationLayer that reads from the given annotation layer name
// The layer_name specifies which annotation namespace to read (e.g., "style", "search")
// priority determines render order (higher = rendered later/on top)
ViewLayer *annot_layer_new(const char *annot_layer_name, int priority);

#endif // WEFT_ANNOT_LAYER_H
