#ifndef WEFT_PLUMB_LAYER_H
#define WEFT_PLUMB_LAYER_H

#include "layer.h"

// PlumbLayer: scans buffer text for clickable patterns
// Recognized patterns:
//   - file.ext:line       -> open file at line (middle-click)
//   - file.ext:line:col   -> open file at line (col ignored for now)
//
// Unlike the old plumber which wrote to AnnotStore, this layer computes
// decorations on-the-fly during rendering. Decorations include:
//   - Underline style (to indicate clickable)
//   - Action script for middle-click that opens the file

// Create a PlumbLayer with the given priority
// priority determines render order (higher = rendered later/on top)
ViewLayer *plumb_layer_new(int priority);

#endif // WEFT_PLUMB_LAYER_H
