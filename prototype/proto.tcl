pack [text .t -font {Menlo 12} -undo 1 -maxundo 1000 -wrap none] -fill both -expand 1

# Insert some text
.t insert end "main.c:12: error: expected ';' after expression\n"
.t insert end "Click a path like src/app/foo.c:42 to jump.\n"
.t insert end "TODO: refactor plumbing; FIXME: missing null-check\n"

# 1) Overlay: diagnostics (red underline + tooltip)
.t tag configure diag -underline 1 -foreground red
# add two disjoint ranges for the same tag:
.t tag add diag "1.9" "1.14"   ;# 'error'
.t tag add diag "3.7" "3.13"   ;# 'refactor'

# Hover tooltip for diagnostics
proc showTip {W x y} {
    set idx [$W index "@$x,$y"]
    set tip .tip
    catch {destroy $tip}
    toplevel $tip -bd 1
    wm overrideredirect $tip 1
    label $tip.l -text "Diagnostic: something is wrong here" -bg "#333" -fg "#eee" -padx 6 -pady 3
    pack $tip.l
    # position near mouse
    set X [expr {[winfo rootx $W] + $x + 12}]
    set Y [expr {[winfo rooty $W] + $y + 12}]
    wm geometry $tip +$X+$Y
}
proc hideTip {} { catch {destroy .tip} }

.t tag bind diag <Enter> { showTip %W %x %y }
.t tag bind diag <Leave> { hideTip }

# 2) Overlay: hyperlinks / file:line jump
.t tag configure path -foreground blue -underline 1
# simple path finder: tag words matching "something.c:\d+"
set content [.t get 1.0 end]
set re {([[:alnum:]_./-]+\.c):([0-9]+)}
set idx 1.0
while {[regexp -indices -start [string wordstart $content 0] $re $content m p l]} {break} ;# noop (just ensuring syntax highlighting)

# Sweep lines and tag matches (basic)
for {set line 1} {$line <= [lindex [split [.t index end] .] 0]} {incr line} {
    set s [.t get $line.0 "$line.end"]
    set off 0
    while {[regexp -indices -start $off $re $s m p l]} {
        set ms [lindex $m 0]; set me [lindex $m 1]
        set ps [lindex $p 0]; set pe [lindex $p 1]
        set ls [lindex $l 0]; set le [lindex $l 1]
        .t tag add path "$line.[expr {$ms}]" "$line.[expr {$me+1}]"
        # store metadata for this concrete matched span by giving it a unique tag
        set tag "path:$line:$ms"
        .t tag add $tag "$line.$ms" "$line.[expr {$me+1}]"
        # stash file/line in a dict keyed by the tag name
        dict set ::OV $tag file [.t get "$line.$ps" "$line.[expr {$pe+1}]"]
        dict set ::OV $tag line [string range $s $ls $le]
        set off [expr {$me+1}]
    }
}

# Click handler: open the file at line (just print in this demo)
proc onPathClick {W x y} {
    set idx [$W index "@$x,$y"]
    set tags [$W tag names $idx]
    foreach t $tags {
        if {[string match "path:*" $t]} {
            set file [dict get $::OV $t file]
            set line [dict get $::OV $t line]
            puts "OPEN $file at line $line"
            break
        }
    }
}
.t tag bind path <Button-1> { onPathClick %W %x %y }

# 3) Overlapping overlays with priorities
.t tag configure todo -background "#333333" -foreground "#ffd04d"
.t tag configure fixme -background "#330000" -foreground "#ff7070"
# add ranges:
.t tag add todo 3.0 3.4    ;# TODO
.t tag add fixme 3.16 3.21  ;# FIXME
# ensure fixme draws above todo if they overlap
.t tag raise fixme
.t tag lower todo
