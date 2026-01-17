# Weft

A structural text editor with semantic annotations.

## What is Weft?

Weft is an Acme-inspired text editor that treats text as more than raw characters. Every span of text can carry metadata—styling, linguistic analysis, provenance tracking, or custom semantic tags. These annotations survive editing: insert text before a highlighted word, and the highlight stays with the word.

### Use Cases

- **Programming**: Syntax highlighting, linting, go-to-definition as annotation layers
- **Linguistics**: Part-of-speech tagging, etymology tracking (Germanic vs. Latinate origins), phoneme analysis
- **Creative Writing**: Narrative structure markup, character voice tracking, revision history
- **Research**: Citation provenance, confidence scores, collaborative human/LLM text building

## Building

### Prerequisites

- CMake 3.14+
- C11 compiler (GCC or Clang)
- Development headers for: FreeType, HarfBuzz

On Debian/Ubuntu:
```bash
sudo apt install build-essential cmake libfreetype-dev libharfbuzz-dev
```

### Build

```bash
cd editor/build
cmake ..
make
```

SDL3 and SDL3_ttf are fetched automatically during the build.

### Run

```bash
./weft                    # Start with demo content showing styled annotations
./weft path/to/file.txt   # Open a file
```

## Current Features

### Editor
- Full cursor navigation (arrows, Home/End, Page Up/Down, Ctrl+arrows for word movement)
- Selection (Shift+movement, click-drag, Shift+click)
- Clipboard (Ctrl+C/X/V)
- Undo/Redo (Ctrl+Z, Ctrl+Shift+Z)
- Multiple panes with tiled layout

### Annotation System
- **Anchor-based spans**: Annotations reference positions that automatically adjust when text is inserted or deleted
- **Metadata**: Key/value pairs with namespaced keys (e.g., `style:fg`, `lint:severity`)
- **Layers**: Group annotations by purpose (style, lint, linguistics, etc.)

### Styled Rendering
Annotations in the `style` layer control text appearance:

| Key | Values | Effect |
|-----|--------|--------|
| `style:fg` | Color name or `#RRGGBB` | Text color |
| `style:bg` | Color name or `#RRGGBB` | Background highlight |
| `style:underline` | `"true"` | Underline decoration |

Named colors: red, green, blue, yellow, cyan, magenta, white, black, orange, purple, pink, gray

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Weft Editor                        │
├─────────────────────────────────────────────────────────┤
│  Layout          │  Pane           │  Pane              │
│  (tiled splits)  │  (editor view)  │  (editor view)     │
├──────────────────┴─────────────────┴────────────────────┤
│                      Editor                             │
│  (cursor, selection, edit ops, undo/redo)               │
├─────────────────────────────────────────────────────────┤
│  Buffer                                                 │
│  ┌─────────────────┐  ┌─────────────────────────────┐   │
│  │    Document     │  │      AnnotStore             │   │
│  │  (gap buffer)   │  │  ┌─────────┐ ┌───────────┐  │   │
│  │                 │  │  │ Anchors │ │ Records   │  │   │
│  └─────────────────┘  │  └─────────┘ └───────────┘  │   │
│                       └─────────────────────────────────┤
├─────────────────────────────────────────────────────────┤
│  Platform + Renderer (SDL3)                             │
└─────────────────────────────────────────────────────────┘
```

### How Annotations Survive Edits

Each annotation span is defined by two **anchors**—positions with a bias:

- **Left-biased anchor**: Stays put when text is inserted at its position
- **Right-biased anchor**: Moves forward when text is inserted at its position

When you insert or delete text, all anchors are transformed:
- Insert: Anchors after the insert point shift forward
- Delete: Anchors in the deleted range collapse; anchors after shift backward

This means if you have "Hello" styled in red and insert "Dear " before it, "Hello" remains red—the annotation automatically adjusts to the new position.

## Roadmap

- [x] Core editor (cursor, selection, clipboard, undo)
- [x] Document storage (gap buffer with line index)
- [x] Annotation store with anchor-based spans
- [x] Styled rendering from annotations
- [x] Tiled pane layout
- [ ] **Lcl scripting** - Tcl-like language for annotation layers
- [ ] Interaction dispatcher (`action:on_click` handlers)
- [ ] Syntax highlighting via Lcl layers
- [ ] File dialogs
- [ ] Search/replace

## License

TBD

## Contributing

This is an early-stage personal project. Feel free to open issues for discussion.
