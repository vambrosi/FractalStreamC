# FractalStream

Interactive pictures of the complex plane for mathematicians. You write a small script
(a few lines for a Mandelbrot set); FractalStream compiles it to a GPU shader (SPIR-V, through
Vulkan; MoltenVK on macOS) and lets you explore it: zoom, pan, pick points, and run tools that
draw orbits or change parameters live.

```
# mandelbrot.fstream: a file without sections is a single viewer; z is the pixel's point
w := 0
while w not escapes do
    w = w^2 + z
end
if .stuck then
    color = .black
else
    color = hsv(.iterations / 50, 0.8, 1)
end
```

## Using it

- Open the app without arguments (or double-click it) to get an editor with an example
  (Mandelbrot and Julia sets side by side). The pictures update as you type. Ctrl+O (Cmd+O)
  opens a script: the dialog lists the folder (typing filters it) and has buttons for your home,
  the examples and the script's folder.
- `fractalstream script.fstream` opens a script. The script pane folds into a thin bar on the
  left (Ctrl+E, Cmd+E on a Mac) and the config pane into one on the right (Ctrl+T); drag the
  edge of a pane to resize it. Ctrl+F finds (and Ctrl+Alt+F replaces) in the script. Changes
  made to the file in another editor are picked up automatically.
- The coordinates of the point under the mouse show at the bottom left of the view. Zooming deep
  (radius below about 1e-4) switches the view to double-single precision automatically.
- Each view remembers where it has been: Back and Forward (Alt+Left / Alt+Right, Cmd+[ / Cmd+]
  on a Mac, or the mouse's side buttons) step through its zooms and pans. Copy location
  (Ctrl+Shift+C) puts the view's centre and radius on the clipboard as script lines, with all
  their digits (`mandel.center = -0.74364388703715 + 0.13182590420533i`); Paste location goes
  there, and the same lines in `@config` open the script at that place.
- Values changed in the config pane (or by tools) are marked with a dot and survive running the
  script again; a button at the top of the pane writes them, and the places of the views, into
  the script (another sets them back).
- Move the mouse to the left edge of a picture for its tools (each view has its own); the menu
  button on top of that strip (or Esc) switches views, splits them, floats them in a corner, lists
  the tools, saves the picture as a PNG file (as on the screen, or four times larger for
  print) and has the view's place (back, forward, copy, paste). F1 lists every key.

Examples are in [`examples/`](examples) (among them [`saddledrop.fstream`](examples/saddledrop.fstream),
the parameter space of complex Hénon maps after the program SaddleDrop); the language is
described in [`docs/language.md`](docs/language.md).

## Command line (for scripts, agents and tests)

```
fractalstream --check SCRIPT                  type-check, print errors (exit status 1 on errors)
fractalstream --render OUT.png [--viewer NAME] [--size WxH] [--cpu] [--set NAME=VALUE] SCRIPT
fractalstream --spirv DIR SCRIPT              write the SPIR-V of each viewer
fractalstream --state SCRIPT                  print the initial value of every global
fractalstream --tool NAME --at X+Yi SCRIPT    run a tool once; print its drawings and the state
```

## Building

`./build.sh` (Linux, macOS), `build.bat` (Windows/MSVC), `./build.sh app` (macOS bundle with
MoltenVK), `./build.sh test` (test suite). Details and requirements: [`docs/building.md`](docs/building.md).

The code is C11 in a unity build (`src/all.c`); third-party code (GLFW, Vulkan headers,
stb_truetype/stb_image_write, DejaVu fonts, Google's Material Icons font) is vendored in
`third_party/`, with its licenses. Architecture notes for
contributors are in [`agents/architecture.md`](agents/architecture.md).
