# FractalStreamC

This is just a version of [FractalStream](https://github.com/matt-noonan/FractalStream) as a
compiler to SPIR-V (GPU intermediate representation). It is heavily based on Matthew Noonan's
original design and code, with a few UI and language differences (tools are part of the script,
there is projective space and animations support, among others).

Check "getting started" for how to explore the app and all its features.

The workflow is essentially: you write a short script, like the one below, and you explore it
by zooming, panning, picking points, and using tools that draw orbits or change parameters as
you go.

```
# By default, the whole file describes what to plot on a z-plane
# The instructions below will be repeated for every pixel in the screen
w := 0
while w not escapes do
    w = w^2 + z
end
color = gray(.iterations / 100)
```

## Download

From the [latest release](../../releases/latest):

- **Mac** (macOS 12 or newer): `FractalStreamC-macos-arm64.zip` for Apple silicon,
  `FractalStreamC-macos-x86_64.zip` for Intel. Unzip it and move FractalStreamC to Applications.
  The app isn't notarized, so the first time macOS won't open it: on macOS 15 or newer, click
  **Done**, then **System Settings → Privacy & Security → Open Anyway**; on older versions,
  right-click the app and choose **Open**. Or install it with this Terminal command, which
  skips that step:
  ```sh
  curl -fL -o /tmp/FractalStreamC.zip "https://github.com/vambrosi/FractalStreamC/releases/latest/download/FractalStreamC-macos-$(uname -m).zip" && rm -rf /Applications/FractalStreamC.app && ditto -x -k /tmp/FractalStreamC.zip /Applications && open /Applications/FractalStreamC.app
  ```
- **Linux** (x86_64; Ubuntu 22.04, Debian 12, Fedora 36 or newer): unpack
  `FractalStreamC-linux-x86_64.tar.gz` and run `fractalstreamc` in it. It needs a Vulkan driver
  (`sudo apt install mesa-vulkan-drivers`, or your GPU vendor's).
- **Windows**: [build it](docs/building.md).

## Getting started

Open the app: it shows a Mandelbrot set, a Julia set and the script that draws them. Edit the
script and the pictures follow. Then go through the examples in order: **Ctrl+O → Examples**
(Cmd on a Mac). F1 lists the keys, F2 shows the [language reference](docs/language.md), and
[docs/app.md](docs/app.md) describes the window.

In a terminal, `fractalstreamc script.fstream` opens a script, and `fractalstreamc --help` lists the
rest (checking scripts, rendering pictures and videos without a window, running a tool).

## Building

`./build.sh` (Linux, macOS), `build.bat` (Windows), `./build.sh test` (tests). Requirements,
details and how releases are made: [docs/building.md](docs/building.md).

## Credits and license

As I mentioned above, FractalStream, the program, its name, and its design are all Matthew Noonan's
[FractalStream](https://github.com/matt-noonan/FractalStream). The templates in the examples folder
are ports of his scripts. The code here is under BSD 3-Clause, copyright Matthew Noonan and Vinicius
Ambrosi ([LICENSE](LICENSE)); the code in `third_party/` keeps its own licenses.
