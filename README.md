# FractalStream

Interactive pictures of the complex plane, for mathematicians: a new version of Matthew Noonan's
[FractalStream](https://github.com/matt-noonan/FractalStream). Write a short script and explore it
on the GPU: zoom, pan, pick points, and use tools that draw orbits or change parameters as you go.

```
# the whole file is one view; z is the pixel's point
w := 0
while w not escapes do
    w = w^2 + z
end
color = gray(.iterations / 100)
```

## Download

From the [latest release](../../releases/latest):

- **Mac** (macOS 12 or newer): `FractalStream-macos-arm64.zip` for Apple silicon,
  `FractalStream-macos-x86_64.zip` for Intel. Unzip it and move FractalStream to Applications.
  The app isn't notarized, so the first time macOS won't open it: on macOS 15 or newer, click
  **Done**, then **System Settings → Privacy & Security → Open Anyway**; on older versions,
  right-click the app and choose **Open**. Or install it with this Terminal command, which
  skips that step:
  ```sh
  curl -fL -o /tmp/FractalStream.zip "https://github.com/vambrosi/FractalStreamC/releases/latest/download/FractalStream-macos-$(uname -m).zip" && rm -rf /Applications/FractalStream.app && ditto -x -k /tmp/FractalStream.zip /Applications && open /Applications/FractalStream.app
  ```
- **Linux** (x86_64; Ubuntu 22.04, Debian 12, Fedora 36 or newer): unpack
  `FractalStream-linux-x86_64.tar.gz` and run `fractalstream` in it. It needs a Vulkan driver
  (`sudo apt install mesa-vulkan-drivers`, or your GPU vendor's).
- **Windows**: [build it](docs/building.md).

## Getting started

Open the app: it shows a Mandelbrot set, a Julia set and the script that draws them. Edit the
script and the pictures follow. Then go through the examples in order: **Ctrl+O → Examples**
(Cmd on a Mac). F1 lists the keys, F2 shows the [language reference](docs/language.md), and
[docs/app.md](docs/app.md) describes the window.

In a terminal, `fractalstream script.fstream` opens a script, and `fractalstream --help` lists the
rest (checking scripts, rendering PNG files without a window, running a tool).

## Building

`./build.sh` (Linux, macOS), `build.bat` (Windows), `./build.sh test` (tests). Requirements,
details and how releases are made: [docs/building.md](docs/building.md).

## Credits and license

FractalStream, the program and its name, is Matthew Noonan's: this version is heavily based on his
[FractalStream](https://github.com/matt-noonan/FractalStream), and its templates are ports of his
scripts. BSD 3-Clause, copyright Matthew Noonan and Vinicius Ambrosi ([LICENSE](LICENSE)); the code
in `third_party/` keeps its own licenses.
