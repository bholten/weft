# Weft

Weft is an Acme-like programmable textual workspace: "structural text
with metadata". 

Not just a code editor: target uses include linguistics, fiction
analysis, programming, and collaborative text work.

Weft is a nearly-pure-[Lcl](https://github.com/bholten/lcl)
application on the [lk](https://github.com/bholten/lk) UI toolkit.

One interpreter is the image (Lisp/CLIM lineage): namespaces are
packages, every evaluation carries an explicit context and leaves an
execution record, and the whole surface -- commands, transforms,
producers, queries, plumbing rules, config -- is Lcl you can read,
run, and redefine live.

## Principles

- **Text is ground truth.**  Annotations attach to it, never replace
  it; they are re-anchored through edits, undo, and reload.
- **Text is the menu.**  Output is clickable text: `weft::commands`,
  `weft::buffers`, `ls`, compiler and grep output -- right-click a
  location and it opens; middle-click a word and it runs.
- **Preview, then decide.**  Edits produced by programs (transforms,
  `weft::q::replace`) arrive as reviewable plans with a diff; apply or
  reject.
- **Core owns meaning; hosts own effects.**  The one host interface is
  a capability registry, so the same core runs windowed, headless
  under test, and (planned) in a browser.

## Status

**SUPER WORK-IN-PROGRESS** -- but usable daily.

## Building

```bash
git clone --recurse-submodules <this repo>
cmake -B build && cmake --build build -j     # sibling ../lk and ../lcl are auto-detected
./build/weft [files...]                       # windowed
./build/weft --headless test/p0-gates.lcl     # any gate suite, from any cwd
```

## The surface

| Key | Does |
|---|---|
| ctrl+s | save |
| ctrl+e / ctrl+shift+d | split right / down (`weft::split <dir>` — left/up put the new pane first) |
| ctrl+alt+arrows | focus the pane in that direction (the workspace is a manual tiler) |
| ctrl+alt+shift+arrows | SEND this buffer to the pane that way — focus and layout stay put |
| ctrl+alt+z | zoom: only this pane, tree intact — toggle; any layout change drops it |
| alt+left / alt+right | back / forward along this pane's own buffer trail |
| ctrl+w · ctrl+n · ctrl+b | close pane (buffers survive) · next pane · cycle buffer |
| ctrl+shift+b · ctrl+1 | show a buffer by name (`qol` finds `docs/qol.md`) · collapse to this pane |
| ctrl+shift+w · ctrl+shift+e | kill the buffer (`weft::close NAME force` discards) · flip the split's axis |
| ctrl+enter | EXECUTE the selection or line |
| ctrl+r · ctrl+t | the Lcl REPL `*lcl*` · a `*shell*` buffer with its own cwd |
| ctrl+o | open or create a file (bar prefilled with the buffer's cwd) |
| ctrl+i · ctrl+shift+i | inspect at the cursor · edit the annotations there as text |
| ctrl+f / ctrl+h / ctrl+g | find / replace / goto line |
| middle-click | EXECUTE the word or selection |
| right-click | LOOK: plumb rules act (file:line opens, urls open…); bare words search |
| shift+right-click | LOOK, opening the result in a new pane beside the source |

## Programming it

Everything below is an Lcl form you can run with ctrl+enter, in
`*lcl*` or in any buffer.

```lcl
;; A query is a plan: source, stages, one terminal.
-> [weft::q::doc] {weft::q::matches {TODO-[0-9]+}} {weft::q::preview 5}
-> [weft::q::doc] {weft::q::lines} {weft::q::select [lambda {r} {...}]} {weft::q::show}
-> [weft::q::doc] {weft::q::matches {colour}} {weft::q::replace [lambda {r} { "color" }]}   ;; pends a diff

;; Live annotation rules are producers: styled, revision-driven.
-> [weft::q::doc] {weft::q::matches {FIXME}} {weft::q::annotate fixme #{fg (255 160 80)}}

;; Transforms: preview, then apply.
weft::transform::register! #{name shout description "upper-case the selection"
    fn [lambda {ctx} { weft::transform::replace [String::upper [get $ctx text]] }]}
weft::transform::run shout        ;; ctrl+u is bound to `run upper` by default

;; Attach the REPL to a buffer: evals then carry it (and its live
;; selection) as ctx.
weft::repl::attach
```

Config lives in `~/.config/weft/init.lcl` and sees the full vocabulary:

```lcl
weft::config::filetype! ".md" #{wrap word}
weft::config::filetype! ".c"  #{lang c}                 ;; tree-sitter highlighting
weft::config::bind! "ctrl+u" {weft::transform::run "upper"}  ;; chords are a host profile: config/bindings-native.lcl
weft::plumb::rule! #{name ticket pattern {TICKET-[0-9]+} filetypes (".md")
    style #{fg (255 200 100)} action [lambda {value ctx} { ... }]}
weft::ts::style! "keyword" #{fg (198 120 221)}         ;; the palette is data
```

## License

MIT.  Roboto Mono is distributed under the SIL Open Font License
(`assets/fonts/Roboto_Mono/OFL.txt`).
