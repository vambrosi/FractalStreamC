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

## Download and run

Ready-to-run downloads for Mac and Linux are on the [Releases page](../../releases/latest): no
compiler, Homebrew or git needed. (Windows: build it yourself, see [`docs/building.md`](docs/building.md).)

### Mac (macOS 12 or newer)

1. Download **FractalStream-macos-arm64.zip** for a Mac with Apple silicon (M1, M2, M3, ...), or
   **FractalStream-macos-x86_64.zip** for a Mac with an Intel processor. Apple menu > About This Mac
   tells which: "Chip: Apple M..." or "Processor: ... Intel ...".
2. Unzip it (double-click the zip, if the browser didn't) and drag **FractalStream** into **Applications**.
3. Open it. The first time, macOS refuses, saying it could not verify that FractalStream is free of
   malware: the app is not notarized by Apple (that needs a paid developer account). To open it anyway:
   - macOS 15 (Sequoia) and newer: click **Done**, then open **System Settings > Privacy & Security**,
     scroll down to the message about FractalStream, click **Open Anyway**, then **Open Anyway** again
     and enter your password.
   - macOS 14 and older: right-click (or Control-click) FractalStream in Applications, choose **Open**,
     then click **Open** in the dialog.

   macOS asks only once; afterwards FractalStream opens like any app.

**Or install it with one command, without that warning**: paste this into Terminal (in
Applications > Utilities) and press Return. It downloads the version for your Mac (`$(uname -m)` is
`arm64` on Apple silicon, `x86_64` on Intel), puts FractalStream in Applications (replacing an older
one) and opens it. Files downloaded by `curl` are not marked as coming from the internet, so macOS has
nothing to ask; no developer tools are needed. The same command updates it later.

```sh
curl -fL -o /tmp/FractalStream.zip "https://github.com/vambrosi/FractalStreamC/releases/latest/download/FractalStream-macos-$(uname -m).zip" && rm -rf /Applications/FractalStream.app && ditto -x -k /tmp/FractalStream.zip /Applications && open /Applications/FractalStream.app
```

(On a Mac where your account is not an administrator, "Permission denied": use `~/Applications`, the
Applications folder in your home folder, instead of `/Applications` in the command; create it first.)
If macOS ever says that FractalStream "is damaged and can't be opened", it is that internet mark
again; this removes it: `xattr -dr com.apple.quarantine /Applications/FractalStream.app`.

### Linux (64-bit PC: Ubuntu 22.04, Debian 12, Fedora 36 or newer)

Download **FractalStream-linux-x86_64.tar.gz**, then in a terminal:

```sh
tar -xzf FractalStream-linux-x86_64.tar.gz
FractalStream-linux-x86_64/fractalstream
```

or both in one go: `curl -fL https://github.com/vambrosi/FractalStreamC/releases/latest/download/FractalStream-linux-x86_64.tar.gz | tar -xz && FractalStream-linux-x86_64/fractalstream`.
The folder can be moved anywhere (the examples and the language reference are in it).
FractalStream needs a Vulkan driver, which most desktops have; if it says "no Vulkan library found",
install one: `sudo apt install libvulkan1 mesa-vulkan-drivers` (Ubuntu, Debian, Mint) or
`sudo dnf install vulkan-loader mesa-vulkan-drivers` (Fedora). NVIDIA's own driver includes Vulkan.

## Using it

- Open the app without arguments (or double-click it) to get an editor with an example
  (Mandelbrot and Julia sets side by side). The pictures update as you type. Ctrl+O (Cmd+O)
  opens a script: the dialog lists the folder (typing filters it, Enter opens the file you
  mean) and has buttons for the recent scripts, your home, the examples and the script's folder.
  Ctrl+N starts a new script from a template. Unsaved changes are asked about before they would
  be lost (opening another script, a new one, or quitting).
- `fractalstream script.fstream` opens a script. The window has a bar on each side. The left bar
  has the script's button (Ctrl+E, Cmd+E on a Mac, shows and hides the script pane next to it),
  then a numbered button for each view of the script, with that view's tools below it (the
  selected one highlighted): a click on a number shows that view (or makes it the one the right
  bar acts on), and dragging it onto the pictures places it — the middle of a view replaces it,
  its sides split it, the corners float it. The right bar has the config's button (Ctrl+T) and
  what you can do with the view you clicked last: back, forward, reset, copy and paste its
  location, axes and grid, save its picture (as on the screen, or four times larger for print),
  hide it; and the list of keys. Drag the edge of a pane to resize it. Ctrl+F finds (and Ctrl+Alt+F replaces) in the script; while you
  type a name, the editor offers the ones it could be (Tab takes one), and resting the mouse on a
  name says what it is. Changes made to the file in another editor are picked up automatically.
- The coordinates of the point under the mouse show at the bottom left of the view; Ctrl+Shift+A
  (or the grid button in the right bar) adds the axes and grid lines with their values. Zoomed
  deep (radius below about 1e-4 for the Mandelbrot set), a view gets blocky in single precision;
  turn on `.doublePrecision` (the note in the view, or the config panel) to compute it in
  double-single precision there (slower, sharp to about 1e-12).
- Each view remembers where it has been: Back and Forward (Alt+Left / Alt+Right, Cmd+[ / Cmd+]
  on a Mac, or the mouse's side buttons) step through its zooms and pans. Copy location
  (Ctrl+Shift+C) puts the view's centre and radius on the clipboard as script lines, with all
  their digits (`mandel.center = -0.74364388703715 + 0.13182590420533i`); Paste location goes
  there, and the same lines in `@config` open the script at that place.
- Values changed in the config pane (or by tools) are marked with a dot and survive running the
  script again; a button at the top of the pane writes them, and the places of the views, into
  the script (another sets them back).
- `print "period " + n` in a tool or in `@prepare` writes in the Info pane (the `i` button in the
  right bar); the Inspect tool (the last button under each view, or `?`) shows the values a view
  computes at the point you click, like FractalStream's Debug tool.
- F1 lists every key; F2 (or the book in the script's toolbar) shows the language reference,
  with a search field (F2 in the script looks up the word under the caret). Ctrl+= / Ctrl+- /
  Ctrl+0 make everything bigger, smaller or normal (for a talk, say).

Examples are in [`examples/`](examples) (among them [`saddledrop.fstream`](examples/saddledrop.fstream),
the parameter space of complex Hénon maps after the program SaddleDrop, and
[`attractors.fstream`](examples/attractors.fstream), the attracting cycles of a rational map on the
Riemann sphere with the parameter plane of Per_2(0)); the language is described in
[`docs/language.md`](docs/language.md). Points of the sphere have their own type, `P`, exact at
infinity, with derivatives and Newton's method in charts.

## Command line (for scripts, agents and tests)

```
fractalstream --check SCRIPT                  type-check, print errors (exit status 1 on errors)
fractalstream --render OUT.png [--viewer NAME] [--size WxH] [--cpu] [--set NAME=VALUE] SCRIPT
fractalstream --spirv DIR SCRIPT              write the SPIR-V of each viewer
fractalstream --state SCRIPT                  print the initial value of every global
fractalstream --tool NAME --at X+Yi SCRIPT    run a tool once; print its drawings and the state
fractalstream --builtins                      print how the built-in functions are defined
```

## Building

`./build.sh` (Linux, macOS), `build.bat` (Windows/MSVC), `./build.sh app` (macOS app with
MoltenVK, and its zip), `tools/make_linux_release.sh` (the Linux download), `./build.sh test` (test
suite). Details and requirements: [`docs/building.md`](docs/building.md), which also says how a release
is made (pushing a version tag: [`.github/workflows/release.yml`](.github/workflows/release.yml) builds
and publishes the downloads).

The code is C11 in a unity build (`src/all.c`); third-party code (GLFW, Vulkan headers,
stb_truetype/stb_image_write, DejaVu fonts, Google's Material Icons font) is vendored in
`third_party/`, with its licenses. Architecture notes for
contributors are in [`agents/architecture.md`](agents/architecture.md).
