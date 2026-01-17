package require Tk
ttk::style theme use clam
wm title . "My Application"

# Layout
set root [ttk::frame .root]
pack $root -fill both -expand 1

# Toolbar
set tb [ttk::frame $root.tb]
pack $tb -side top -fill x

ttk::button $tb.open -text "Open" -command {puts "open"}
pack $tb.open -side left

ttk::button $tb.quit -text "Quit" -command {destroy .}
pack $tb.quit -side right

# Content
set content [ttk::frame $root.content]
pack $content -fill both -expand 1

ttk::label $content.l -text "Name:"
grid $content.l -row 0 -column 0 -sticky e -padx 6 -pady 6

set ::name ""
ttk::entry $content.e -textvariable ::name

grid $content.e -row 0 -column 1 -sticky we -padx 6 -pady 6
grid columnconfigure $content 1 -weight 1

#Status bar
set sb [ttk::label $root.sb -text "Ready" -anchor w]
pack $sb -side bottom -fill x

# Key bindings
event add <<Quit>> <Control-q>
bind . <<Quit>> {destroy .}
