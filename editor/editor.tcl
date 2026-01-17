package require Tk

pack [text .t -wrap word] -fill both -expand 1

.t configure -undo true -autoseparators true
.t insert end "This is a simple text editor example in Tcl/Tk.\n"
.t insert end "You can type and edit text here.\n"

.t tag configure title -font "helvetica 14 bold" -justify center
.t insert 1.0 "My Document Title\n" title

set menubar [menu .menubar]
. configure -menu $menubar

set fileMenu [menu $menubar.file -tearoff 0]

$menubar add cascade -label "File" -menu $fileMenu

$fileMenu add command -label "Save" -command {
    tk_messageBox -message "Save functionality not implemented in this example."
}

$fileMenu add command -label "Exit" -command {exit}
