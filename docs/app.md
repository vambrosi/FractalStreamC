# The window

Cmd instead of Ctrl on a Mac. F1 lists every key.

- **Scripts.** Without a script, the app opens an example in the editor. The pictures follow as you
  type. Ctrl+O opens a script (type to filter; buttons for recent scripts, home, the examples and the
  script's folder), Ctrl+N starts one from a template, Ctrl+S saves. Unsaved changes are asked about
  before they'd be lost; changes made in another editor are picked up.
- **Left bar.** The script button (Ctrl+E shows or hides the editor), then a numbered button for each
  view, with its tools below. Click a number to show that view; drag it onto the pictures to place
  it: the middle replaces a view, the sides split it, the corners float it.
- **Right bar.** The config (Ctrl+T), the Info pane, and what you can do with the view you clicked
  last: back and forward, reset, copy and paste its location, axes and grid, save its picture, hide it.
- **Panes** float over the views. The layers button at the bottom of the right bar puts them beside
  the views instead. Drag a pane's edge to resize it.
- **Editor.** Ctrl+F finds, Ctrl+Alt+F replaces, Tab completes a name. Resting the mouse on a name
  says what it is; F2 looks up the word under the caret in the language reference.
- **Views.** The point under the mouse shows at the bottom left; Ctrl+Shift+A adds axes and a grid.
  Alt+Left / Alt+Right (Cmd+[ / Cmd+]) step through the zooms and pans. Copy location (Ctrl+Shift+C)
  gives `NAME.center = ...` and `NAME.radius = ...` lines; in `@config` they open the script there.
- **Antialiasing.** Once a view rests, it is drawn again a few times with the samples moved inside
  the pixels, and averaged: fine detail gets smooth. `.antialiasing` sets how many samples (default
  8; 1: off).
- **Deep zooms** (radius below about 1e-4) get blocky: turn on `.doublePrecision` (in the note on the
  view, or in the config) for double-single precision, sharp to about 1e-12. It is slower.
- **Config.** Values changed in the config pane, or by tools, get a dot and survive running the script
  again. A button writes them, and where the views are, into the script.
- **Deleting a view's section** closes that view; Undo brings it back.
- **Info.** `print` (in a tool or in `@prepare`) writes in the Info pane. The Inspect tool (`?`) shows
  the values a view computes at the point you click.
- **Size.** Ctrl+= / Ctrl+- / Ctrl+0 make everything bigger, smaller, or normal (for a talk).
