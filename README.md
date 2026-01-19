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

SDL3, SDL3_ttf, and tree-sitter are fetched automatically during the build.

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

**Keybindings**:
```tcl
;; Override Ctrl+W to close pane via script
weft::config::keybind "ctrl+w" { weft::pane::close }

;; Pane navigation
weft::config::keybind "ctrl+tab" { weft::pane::next }
weft::config::keybind "ctrl+shift+tab" { weft::pane::prev }

;; Function key shortcuts
weft::config::keybind "f5" {
    let result [weft::shell::exec "make"]
    weft::buffer::open-output "*build*" [List::get $result 1]
}
```

Keybinding key-string format: `modifier+modifier+key` where modifiers are `ctrl`, `shift`, `alt` and keys can be `a`-`z`, `0`-`9`, `f1`-`f12`, or named keys like `return`, `escape`, `tab`, `space`, etc.

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
| Ctrl+W | Close current pane |
| Ctrl+Tab | Cycle focus between panes |
| Ctrl+` | Open Lcl REPL |
| Ctrl+I | Inspect annotations at cursor |
| Ctrl+Enter | Execute selection or line |
| Tab | Insert tab or spaces (per file type settings) |
| Escape | Close search/dialogs |

## Lcl REPL

Press **Ctrl+`** to open the Lcl scripting console. This is an interactive environment for:

- Testing Lcl expressions
- Querying document state (`weft::doc::*`)
- Experimenting with annotation commands
- Debugging layer scripts

The REPL features **tree-sitter syntax highlighting** for Lcl code. Keywords, strings, comments, variables, and brackets are all colored appropriately.

Type an expression and press **Ctrl+Enter** to evaluate. Results appear inline:

```
[+ 1 2]
;; => 3

[weft::version]
;; => Weft 0.1.0

[weft::doc::line-count]
;; => 42
```

Errors are shown in red:
```
[undefined-command]
;; ERROR: unknown command: undefined-command
```

## Annotation Inspector

Press **Ctrl+I** to inspect all annotations at the cursor position. This opens a new buffer showing:

- Position information (byte offset, line, column)
- Each annotation's ID, layer, and span
- The annotated text (preview)
- All metadata key/value pairs

Example output:
```
;; Annotations at position 1234
;; Line 45, Column 12
;;
;; Found 2 annotation(s):
;;
─────────────────────────────────────────
Annotation #7
  Layer: style
  Span:  [1230, 1250) (20 bytes)
  Text:  "function_name"
  Source: ~/.config/weft/syntax_c.lcl
  Metadata:
    style:fg = "#88CC88"
    style:underline = "true"

─────────────────────────────────────────
Annotation #12
  Layer: action
  Span:  [1230, 1250) (20 bytes)
  Text:  "function_name"
  Source: ~/.config/weft/syntax_c.lcl
  Metadata:
    action:on_middle_click = "weft::buffer::open-at \"/path/to/file.c\" 100"
```

The **Source** field shows which script created each annotation (provenance tracking).

You can also use `weft::inspect::at <position>` from the REPL or in scripts.

## Tree-sitter Syntax Highlighting

Weft integrates [tree-sitter](https://tree-sitter.github.io/) for fast, accurate AST-based syntax highlighting. Tree-sitter provides:

- **Incremental parsing**: Only re-parses changed portions of the document
- **AST-based highlighting**: Colors are based on actual syntax structure, not regex patterns
- **Extensible**: Add new languages by providing a tree-sitter grammar and highlight queries

### Using Tree-sitter

The `weft::ts::highlight` command applies tree-sitter highlighting to the current buffer:

```tcl
;; In a layer script (e.g., syntax_lcl_ts.lcl)
weft::ts::highlight "/path/to/queries/highlights.scm"
```

Currently supported languages:
- **Lcl** (`.lcl` files) - Uses the tree-sitter-lcl parser

### Adding New Languages

To add tree-sitter support for a new language:
1. Obtain or create a tree-sitter grammar for the language
2. Create a `highlights.scm` query file mapping syntax nodes to highlight groups
3. Create a layer script that calls `weft::ts::highlight` with the query path
4. Register the layer in `~/.config/weft/init.lcl`

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
- [x] Lcl REPL for interactive scripting
- [x] Annotation inspector (Ctrl+I)
- [x] Tree-sitter integration for AST-based syntax highlighting
- [ ] More syntax highlighting layers
- [ ] Auto-trigger layers on document change (with dirty tracking)
- [ ] Fuel-limited sandboxing for untrusted layers

## License

TBD

## Contributing

This is an early-stage personal project. Feel free to open issues for discussion.
