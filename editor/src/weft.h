#ifndef WEFT_H
#define WEFT_H

#include <stdbool.h>

#include <SDL3/SDL.h>

// Forward declarations
typedef struct Platform Platform;
typedef struct Renderer Renderer;
typedef struct document Document;
typedef struct Annotation Annotation;
typedef struct Editor Editor;

// Main application context
typedef struct {
    Platform *platform;
    Renderer *renderer;
    Document *document;
    Annotation *annotations;
    Editor *editor;
    bool running;
} WeftApp;

#endif // WEFT_H
