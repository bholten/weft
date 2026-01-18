# Weft Layer Scripts

Layer scripts are Lcl programs that analyze document content and create annotations.
This guide explains how to write layer scripts for Weft.

## Overview

Layers are **pure functions** over document content. They:
1. Read the document text using the `weft::doc::*` API
2. Analyze the content (looking for patterns, syntax, etc.)
3. Create annotations using the `weft::annot::*` API

Layers run automatically when the document changes (after a short debounce delay).
Each time a layer runs, its previous annotations are cleared first.

## Quick Example

```lcl
;; Simple layer that highlights the word "TODO" in red
let doc_len [weft::doc::length]
var pos 0

while {< $pos $doc_len} {
    let text [weft::doc::text $pos 4]
    if {eq $text "TODO"} {
        weft::annot::add $pos [+ $pos 4] "my_layer" "style:fg" "red"
    }
    set! pos [+ $pos 1]
}
```

## Document API (`weft::doc::*`)

| Command | Description |
|---------|-------------|
| `weft::doc::length` | Returns document length in bytes |
| `weft::doc::line-count` | Returns number of lines |
| `weft::doc::text start len` | Returns text starting at `start`, up to `len` bytes |
| `weft::doc::line-start n` | Returns byte offset where line `n` begins |
| `weft::doc::line-end n` | Returns byte offset where line `n` ends (before newline) |

### Example: Iterate by Line

```lcl
let line_count [weft::doc::line-count]
var line 0

while {< $line $line_count} {
    let start [weft::doc::line-start $line]
    let end [weft::doc::line-end $line]
    let text [weft::doc::text $start [- $end $start]]

    ;; Process the line...

    set! line [+ $line 1]
}
```

## Annotation API (`weft::annot::*`)

| Command | Description |
|---------|-------------|
| `weft::annot::add start end layer ?key val ...?` | Create annotation, returns ID |
| `weft::annot::remove id` | Delete annotation by ID |
| `weft::annot::at pos ?layer?` | List annotation IDs at position |
| `weft::annot::in-range start end ?layer?` | List annotation IDs overlapping range |
| `weft::annot::get id` | Get annotation details as list |
| `weft::annot::clear-layer layer` | Remove all annotations in layer |

### Creating Annotations

```lcl
;; Basic annotation with foreground color
weft::annot::add 0 10 "my_layer" "style:fg" "blue"

;; Multiple style properties
weft::annot::add 0 10 "my_layer" "style:fg" "yellow" "style:underline" "true"

;; Background color
weft::annot::add 0 10 "my_layer" "style:bg" "#442200"
```

## Style Keys

These keys control visual rendering:

| Key | Values | Description |
|-----|--------|-------------|
| `style:fg` | Color name or `#RRGGBB` | Foreground (text) color |
| `style:bg` | Color name or `#RRGGBB` | Background color |
| `style:underline` | `"true"` or `"1"` | Underline the text |

### Named Colors

Weft recognizes these color names: `red`, `green`, `blue`, `yellow`, `cyan`,
`magenta`, `orange`, `white`, `black`, `gray`.

For precise colors, use hex format: `"#RRGGBB"` (e.g., `"#6699FF"`).

## Lcl Language Basics

Lcl is a Tcl-like language with lexical scoping.

### Variables

```lcl
let x 42          ;; Immutable binding
var y 0           ;; Mutable variable
set! y [+ $y 1]   ;; Mutate y
```

### Procedures

```lcl
proc greet {name} {
    puts (str "Hello, " $name "!")
}

greet "World"
```

### Control Flow

```lcl
;; Conditionals - NOTE: condition uses [...] not {...}
if [> $x 10] {
    puts "x is large"
} else {
    puts "x is small"
}

;; Loops - condition is a VALUE, use [...] for expressions
while [< $i 100] {
    ;; body
    set! i [+ $i 1]
}

foreach item $list {
    puts $item
}
```

### Useful String Functions

```lcl
String::length $str       ;; Length of string
String::index $str $i     ;; Character at index i
String::find $str $sub    ;; Find substring, returns index or -1
String::range $str $s $e  ;; Substring from s to e (exclusive)
String::upper $str        ;; Convert to uppercase
String::lower $str        ;; Convert to lowercase
String::split $str $sep   ;; Split string by separator
String::join $lst $sep    ;; Join list with separator
eq $a $b                  ;; String equality
```

## Best Practices

### 1. Use Meaningful Layer Names

Choose descriptive layer names that don't conflict:
- `"syntax"` - Syntax highlighting
- `"lint"` - Linting/error markers
- `"search"` - Search match highlighting

### 2. Keep Annotations Focused

Each layer should have a single responsibility. Don't mix syntax highlighting
with linting in the same layer.

### 3. Word Boundary Checking

When highlighting keywords, check word boundaries to avoid partial matches:

```lcl
;; Characters that can be part of a word
let word_chars "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"

proc is_word_char {ch} {
    let idx [String::find $word_chars $ch]
    return [>= $idx 0]
}

proc at_word_start {pos} {
    if {eq $pos 0} { return 1 }
    let prev [weft::doc::text [- $pos 1] 1]
    return [not [is_word_char $prev]]
}
```

### 4. Handle Edge Cases

- Empty documents (`doc::length` = 0)
- Unterminated strings/comments at end of file
- Unicode characters (work in bytes, not characters)

### 5. Performance Tips

- Avoid creating unnecessary annotations
- For large files, consider processing line-by-line
- Use `break` to exit loops early when possible

## Example: Simple TODO Highlighter

```lcl
;; Highlight TODO, FIXME, and HACK comments
let patterns [list "TODO" "FIXME" "HACK" "XXX"]
let colors [list "#FF8800" "#FF0000" "#FFFF00" "#FF00FF"]

let doc_len [weft::doc::length]
var pos 0

while {< $pos $doc_len} {
    var idx 0
    foreach pattern $patterns {
        let plen [String::len $pattern]
        if {<= [+ $pos $plen] $doc_len} {
            let text [weft::doc::text $pos $plen]
            if {eq $text $pattern} {
                let color [List::get $colors $idx]
                weft::annot::add $pos [+ $pos $plen] "todo" "style:fg" $color
                set! pos [+ $pos $plen]
                break
            }
        }
        set! idx [+ $idx 1]
    }
    set! pos [+ $pos 1]
}
```

## File Structure

```
layers/
  syntax_c.lcl       # C syntax highlighting
  highlight_demo.lcl # Simple demo
  README.md          # This file
```

## Registering Layers

Layers are registered in `weft.c` at startup:

```c
// Register a layer with the Lcl host
lcl_host_register_layer(host, "syntax", "/path/to/syntax_c.lcl");

// Register in annotation store and mark dirty for initial run
annot_store_register_layer(buffer->annots, "syntax");
annot_store_set_layer_dirty(buffer->annots, "syntax");
```

Registered layers automatically refresh when the document changes, with a
300ms debounce delay to avoid running on every keystroke.
