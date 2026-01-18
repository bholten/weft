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
- Search (Ctrl+F) and Replace (Ctrl+H)
- Go to line (Ctrl+G)
- Tab handling with configurable tab size and spaces/tabs preference

### Acme-like Interaction
- **Middle-click execution**: Click on text to execute it
  - File paths (`/path/to/file`) open in a new pane
  - `file.c:42` patterns open file at specific line
  - Shell commands execute and show output in a scratch buffer
  - `weft::*` commands execute as Lcl scripts
- **Ctrl+Enter**: Execute selected text or current line (keyboard middle-click)
- **Annotation handlers**: Annotations can define custom click behavior via `action:on_middle_click`

### Annotation System
- **Anchor-based spans**: Annotations reference positions that automatically adjust when text is inserted or deleted
- **Metadata**: Key/value pairs with namespaced keys (e.g., `style:fg`, `lint:severity`)
- **Layers**: Group annotations by purpose (style, lint, action, etc.)

### Styled Rendering
Annotations in the `style` layer control text appearance:

| Key | Values | Effect |
|-----|--------|--------|
| `style:fg` | Color name or `#RRGGBB` | Text color |
| `style:bg` | Color name or `#RRGGBB` | Background highlight |
| `style:underline` | `"true"` | Underline decoration |

Named colors: red, green, blue, yellow, cyan, magenta, white, black, orange, purple, pink, gray

### Configuration

Weft loads configuration from `~/.config/weft/init.lcl` at startup. This file can configure:

**File type associations** (syntax highlighting):
```tcl
weft::config::filetype ".c" "style" "/path/to/syntax_c.lcl"
weft::config::filetype ".py" "style" "/path/to/syntax_python.lcl"
```

**Indent settings** (tab size and tabs vs spaces):
```tcl
;; C files: 4-character tabs, use actual tabs
weft::config::indent ".c" 4 1

;; Python: 4 spaces, use spaces (not tabs)
weft::config::indent ".py" 4 0

;; JavaScript: 2 spaces
weft::config::indent ".js" 2 0
```

**Custom handlers**:
```tcl
weft::config::handler "my-handler" "middle-click" {
    puts "Custom handler triggered!"
    weft::interact::handled
}
```

### Lcl Scripting

Weft embeds [Lcl](submodules/lcl), a Tcl-like language with lexical scoping, for scriptable annotation layers and interaction handlers.

**Document bindings:**
- `weft::doc::length`, `line-count`, `text`, `line-start`, `line-end`

**Annotation bindings:**
- `weft::annot::add`, `remove`, `at`, `in-range`, `get`, `clear-layer`

**Interaction bindings:**
- `weft::interact::event-type`, `position`, `selection`, `word-at-pos`, `modifiers`, `handled`

**Editor bindings:**
- `weft::editor::insert`, `delete`, `cursor`, `cursor-set`

**Buffer bindings:**
- `weft::buffer::open path` - Open file in new pane
- `weft::buffer::open-at path line` - Open file and jump to line
- `weft::buffer::open-output name content` - Create scratch buffer

**Shell bindings:**
- `weft::shell::exec cmd` - Execute shell command, returns `{status stdout stderr}`

**Filesystem bindings:**
- `weft::fs::ls ?path?` - List directory contents with clickable entries

**Plumber bindings:**
- `weft::plumb::scan` - Scan buffer for file:line patterns, add clickable annotations

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Weft Editor                        │
├─────────────────────────────────────────────────────────┤
│  Layout          │  Pane           │  Pane              │
│  (tiled splits)  │  (editor view)  │  (editor view)     │
├──────────────────┴─────────────────┴────────────────────┤
│                      Editor                             │
│  (cursor, selection, edit ops, undo/redo, indent)       │
├─────────────────────────────────────────────────────────┤
│  Buffer                                                 │
│  ┌─────────────────┐  ┌─────────────────────────────┐   │
│  │    Document     │  │      AnnotStore             │   │
│  │  (gap buffer)   │  │  ┌─────────┐ ┌───────────┐  │   │
│  │                 │  │  │ Anchors │ │ Records   │  │   │
│  └─────────────────┘  │  └─────────┘ └───────────┘  │   │
│                       └─────────────────────────────────┤
├─────────────────────────────────────────────────────────┤
│  LclHost (Lcl Interpreter)                              │
│  ┌───────────┐ ┌───────────┐ ┌──────────┐ ┌──────────┐  │
│  │ weft::doc │ │weft::annot│ │weft::shell│ │ Config   │  │
│  └───────────┘ └───────────┘ └──────────┘ └──────────┘  │
├─────────────────────────────────────────────────────────┤
│  Interaction Dispatcher                                 │
│  (middle-click → annotation handlers → global handlers) │
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

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl+C/X/V | Copy/Cut/Paste |
| Ctrl+Z | Undo |
| Ctrl+Shift+Z | Redo |
| Ctrl+A | Select all |
| Ctrl+F | Find |
| Ctrl+H | Find and Replace |
| Ctrl+G | Go to line |
| Ctrl+Enter | Execute selection or line |
| Tab | Insert tab or spaces (per file type settings) |
| Escape | Close search/dialogs |

## Roadmap

- [x] Core editor (cursor, selection, clipboard, undo)
- [x] Document storage (gap buffer with line index)
- [x] Annotation store with anchor-based spans
- [x] Styled rendering from annotations
- [x] Tiled pane layout
- [x] Lcl scripting with document/annotation bindings
- [x] Layer execution (scripts create annotations)
- [x] Search/replace
- [x] Interaction dispatcher (middle-click execution)
- [x] Shell command execution
- [x] Configuration file (~/.config/weft/init.lcl)
- [x] File type associations for syntax highlighting
- [x] Indent settings per file type
- [x] Plumber (file:line pattern detection)
- [ ] More syntax highlighting layers
- [ ] Auto-trigger layers on document change (with dirty tracking)
- [ ] Fuel-limited sandboxing for untrusted layers

## License

TBD

## Contributing

This is an early-stage personal project. Feel free to open issues for discussion.
