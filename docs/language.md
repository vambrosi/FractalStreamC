# The FractalStream language

A FractalStream script describes interactive pictures of the complex plane. It has
**viewers** (code that runs for every pixel and produces a colour), **tools** (code that runs
when you click in a viewer; it can change variables and draw shapes), **globals** (state shared
by everything) and **config** (settings and variables you can tweak in the GUI).

```
# Mandelbrot set: the whole file is one viewer, `z` is the pixel's point
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

## Sections

```
@config                       GUI settings, and variables shown in the config panel
@global                       state and functions shared by everything
@prepare                      runs whenever the state changes, before the pictures are drawn
@viewer NAME VAR ["Display name"] ["Shortcut"]
@tool ["Display name"] ["Shortcut"]
```

- A file without section headers is one viewer: `@viewer plane z "z-plane" "Ctrl + 1"`.
- Code before the first header is treated as `@global`.
- `@global` and `@config` run once, in file order, when the script is loaded. The variables they
  declare are the program **state**. Config variables also get a control in the config panel.
- `@prepare` runs (on the CPU) after them, and again whenever the state changes (a config value, a
  tool, a picked point, the view; not the mouse or the time), before the pictures are drawn. Use it
  for values the viewers need that are computed once from the state instead of at every pixel,
  such as a fixed point that depends on a parameter. It can change globals; keep it quick, or
  guard it with an `if` that checks whether its inputs changed.
- In a viewer, `VAR` is the point of the current pixel (complex). The viewer sets `color`
  (it starts black). `return` ends the viewer early.
- Every viewer `NAME` with variable `VAR` adds three state variables: `.VAR` (the last point picked
  in it, see *Pick point* below), `NAME.center` and `NAME.radius` (the view: `NAME.radius` is the
  radius of the largest disk around the centre that fits in the view; defaults 0 and 2).
- Adding `projective` at the end of a viewer header shows the whole **Riemann sphere**, seen from
  its centre: `@viewer sphere z "Riemann sphere" "Ctrl + 3" projective`. The variable is still a
  complex number (it can be huge near infinity). Dragging rotates the sphere, the wheel zooms around
  the point under the mouse (a hyperbolic Möbius map fixing it and its antipode). `NAME.center` and
  `NAME.radius` set the initial view; the current view is the Möbius map `NAME.mobius`.
- Shortcuts are written like `"Ctrl + 1"`, `"Shift + P"`, `"O"`. On macOS, Ctrl means Cmd.
  Viewers without a shortcut get Ctrl + 1, Ctrl + 2, ... in order.

## Values and types

| type | meaning | literals / constructors |
|---|---|---|
| `Z` (`Integers`) | 32-bit integers | `42` |
| `R` (`Reals`) | real numbers (32-bit floats) | `2.5`, `1e-3`, `pi`, `e`, `tau` |
| `C` (`Complex`) | complex numbers | `3i`, `2 + 0.5i`, `complex(x, y)`, `cis(t)`, `polar(r, t)` |
| `H` (`Quaternions`) | quaternions | `quat(a, b, c, d)`, `.j`, `.k` |
| `Bool` | `true` / `false` | comparisons, `and`, `or`, `not` |
| `R^3`, `C^2`, `Z^5`, ... | fixed-size vectors | `[1, 2, 3]` |
| `Color` | same as `R^3` (red, green, blue in 0..1) | `rgb(r, g, b)`, `hsv(h, s, v)`, `gray(v)`, `.red` |
| `List of C` (etc.) | list of up to 64 numbers (`List of C up to 1000`: up to 1000) | `[]`, `[1, i]`, `append(L, x)` |

`i` is the imaginary unit and can't be used as a variable name.

**Promotion.** Numbers convert automatically along `Z -> R -> C -> H` (and elementwise for
vectors), never backwards: use `re`, `im`, `|z|`, `floor`, `ceil`, `round` to go down.

**Precision.** Reals are 32-bit floats (GPUs on macOS have no doubles). When you zoom into a flat
view until a pixel is smaller than about a millionth of the numbers in view, the view is computed
again in double-single precision (pairs of floats, about 14 digits) — slower, but sharp down to a
radius of about `1e-12` times the size of the numbers. The coordinates at the bottom of the view
say "(double precision)" then. Square roots and the four operations get the extra digits; other
functions (`exp`, `log`, `sin`, ...) stay in single precision, which is what colours need. Tools,
`@prepare` and `@global` run on the CPU in single precision.

## Variables

```
x : R = 2.5          # declaration with a type
n : Z                # declared, starts at 0
z := 1 + i           # declaration with the type of the value
w = z^2              # assignment; declares w if no variable named w exists yet
w += 1               # also -=, *=, /=
v[2] = 0             # vectors, lists, and the parts of C (z[1] = re, z[2] = im) and H
(a, b) := f(x)       # unpack a function's results
(x, y) = (x^2 + c - a y, x)   # parallel assignment: every value is computed before any is stored
```

- **Untyped declarations take the type of their value, but plain numbers are complex.**
  `z := 0` and `t := 2.5` declare complex numbers (so `n := 0` can't count a `for` loop: write
  `n : Z = 0`), while `r := |z|` is real, `k := n + 1` is an integer when `n` is, and
  `(n, ok) := f(x)` keeps the types of `f`'s results. Untyped vectors are at least real
  (`v := [1, 2]` is `R^2`).
- **Scopes.** A section body and a function body are scopes; `if`/`for`/`while` bodies are not.
  A `for` loop always declares its own counter.
- Viewers can change globals, but only their own per-pixel copy. Tools change the real state.

## Expressions

Precedence, loosest first: `or`, `and`, `not`, comparisons / `escapes` / `vanishes`, `+ -`,
`* / mod` and implicit multiplication, unary `-`, `^` (right associative), indexing `v[k]`.

- **Implicit multiplication:** `2z`, `2(1 - w)`, `z w`, `3|z|`, `2 pi i t`. A number on the
  right is not allowed (`z 2` is an error).
- **Function application without parentheses:** `cos w + 1` is `cos(w) + 1`; `sin 2 pi t` is
  `sin(2 pi t)`; `sin x cos x` is `sin(x) * cos(x)`; `exp -z^2` is `exp(-(z^2))`;
  `sqrt z / 2` is `sqrt(z) / 2`. Use parentheses when in doubt.
- `/` always divides exactly: `1/2` is `0.5`. `a mod b` is the mathematical remainder
  (same sign as `b`).
- `^`: `z^2`, `z^-1` (integer constants are exact repeated multiplication), `x^0.5`,
  `z^w`, `i^i` (principal branch).
- `|x|` is the absolute value, the modulus of a complex number or quaternion, the length of a
  vector, or the number of items of a list.
- `==` and `!=` on reals, complex numbers, quaternions and vectors mean "closer than
  `.epsilon`". `<`, `<=`, `>`, `>=` only work on `Z` and `R`.
- `z escapes` means `|z| > .escapeRadius`; `z vanishes` means `|z| < .epsilon` (or `z` is 0).
  Also `z not escapes`, `z not vanishes`.
- `px` after a number is a length in pixels of the viewer: `8 px` is `8 .pixel`, as in
  `if |z - a| < 8 px then`.
- `if c then a else b` is a value: `color = if .stuck then .black else rainbow(.iterations / 30)`.
  Only the chosen side is computed; `else if` chains work; the branches extend as far as possible,
  so put the whole `if` in parentheses inside a larger expression.
- Indices start at 1; `L[-1]` is the last entry, `L[-2]` the one before. Out-of-range indices are
  clamped to the first/last entry.

## Statements

```
if c then ... else if c2 then ... else ... end
while c do ... end
do ... while c end                 # body runs at least once
for k in 1..10 do ... end          # 1, 2, ..., 10 (inclusive)
for k in 10..1 by -3 do ... end    # 10, 7, 4, 1
for x in L do ... end              # items of a list or vector
break    continue    return
```

`while` and `do` loops stop after `.maxIterations` iterations; `for` loops run their whole range
(up to a million steps). After a loop, `.stuck` tells whether it was stopped by its limit, and
`.iterations` how many times its body ran.

## Functions

```
define f(z) = z^2 + c                    # one expression; untyped parameters are C
define g(x : R, n : Z) = x^n
define h(x, y) = (u, w) where            # several results, computed by a block
    u = x + y
    w = 0
    if u vanishes then
        return                           # leave with the current u, w
    end
    w = 1 / u
end
(p, q) := h(1, 2)
define mark(z) where                     # no result: called on a line of its own, for what it
    draw point at z                      # does (draw, change globals...)
    draw circle at z with radius 5 px
end
mark(0.5i)
```

Functions see the global state (and may change it), their parameters, their results and their own
variables; not the variables of the section that calls them. Functions with the same name but
different parameter types are allowed (the closest match is used). Recursion is not supported.

## Derivatives and Newton's method

```
derivative z -> z^3 + sin(z) at 2          # the value of d/dz (z^3 + sin z) at z = 2
root z -> z^3 - 1 with seed 1 + i          # a zero, by Newton's method from the seed
critical z -> z^2 + c with seed 1          # a critical point (a zero of the derivative)
critical f with seed 0                     # also with the name of a function of one variable
```

Derivatives are exact: they are computed along with the values (forward-mode automatic
differentiation, to second order) through `+ - * /`, powers,
`exp log sqrt sin cos tan sinh cosh tanh asin acos atan`, and through your functions, loops and
`if`s included:

```
define escape(z, c) = w where
    w = z
    while w not escapes do
        w = w^2 + c
    end
end
w := critical t -> log(escape(t, c)) with seed 1     # a critical point of the escape rate
```

Other variables in the expression are constants for the derivative. Conditions may use anything
(`|w|`, `re`, comparisons, `escapes`), but the values computed from the variable must stay
holomorphic: `re`, `im`, `conj`, `|z|` and `arg` can't be differentiated.

After `root` and `critical`, `.stuck` is true if Newton's method did not converge within
`.maxIterations` steps, and `.iterations` is the number of steps. Newton's method also gives up
when evaluating the expression gets stuck (one of its loops reaches `.maxIterations`: the value
would mean nothing), and it stops successfully when its steps stop shrinking at the level of the
rounding noise of 32-bit floats.

### Following a solution from pixel to pixel

In a viewer, `continuing from c = P` follows the solution across the picture instead of starting
from the seed at every pixel:

```
@viewer param c "Parameter plane"
w := critical t -> log(escape(t, c)) with seed w0 continuing from c = c0
```

- The pixel of the point `P` (a value that is the same for every pixel: globals and constants)
  starts Newton's method from the seed. Every other pixel starts from the solution of a
  neighbour that is already done, moving `c` from the neighbour's point to its own in small
  steps. The fill grows outward from `P` and goes around pixels where Newton's method fails
  (they pass nothing on); pixels it can't reach at all start from the seed.
- A solution with several branches (like the critical points in SaddleDrop) is followed
  continuously; where the fill comes back around an obstacle with another branch, the picture
  shows a line. See [`examples/saddledrop.fstream`](../examples/saddledrop.fstream).
- The fill travels along rays from `P`. Rows of pixels that pass between two nearby branch
  points come back with another branch and would carry it to the edge of the picture as a thin
  band. After the fill, such bands are solved again from their sides ("mending"): a band whose
  two sides agree takes their branch, and a band between two sides that disagree goes to the
  nearer side, so one line is left instead of two.
- One `continuing` solve per viewer, in flat (not projective) viewers. The expression can't use
  locals of the viewer computed from `c` (compute them inside the expression, in a function).
- In the GUI the picture grows from `P` as it is computed.

## Built-in functions

| | |
|---|---|
| real and complex | `sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh exp log ln sqrt` |
| real only | `atan2(y, x)` (also `atan(y, x)`), `log2 log10 floor ceil round trunc fract sign min max clamp mix smoothstep step isnan isinf` |
| complex | `re im arg conj norm abs complex(x, y) cis(t) polar(r, t)` |
| quaternions | `quat(a, b, c, d)`, `conj`, `abs`, `q[1]` ... `q[4]` |
| vectors / lists | `dot(u, v)`, `length(v)`, `append(L, x)`, `clear(L)` |
| colours | `rgb(r, g, b)`, `hsv(h, s, v)` (hue in turns: 0 red, 1/3 green, 2/3 blue), `gray(v)`, `mix(a, b, t)` |
| palettes | `rainbow(t)`, `palette("name", t)` with the maps `rainbow arenberg romao bamo broco corko viko` (cyclic) and `ice fire wheat rose forest ocean winter spring summer fall` (there and back); all repeat with period 1 in `t` |
| from the first FractalStream | `cycle(t, a, b)` (b, then a at t = 1/2, back to b), `blend(t, a, b)` (= `mix(b, a, t)`), `dark(c)`, `light(c)`, `invert(c)`; a script may define its own versions of these and of `rainbow` |

Constants: `pi`, `e`, `tau` (can be shadowed by your own variables), colours `.red .green .blue
.black .white .gray .yellow .cyan .magenta .orange .purple .pink .brown .violet`, quaternion units
`.j .k`.

## Internals (names starting with a dot)

| name | type | meaning |
|---|---|---|
| `.maxIterations` | Z | iteration limit of `while` and `do` loops (default 1000) |
| `.epsilon` | R | tolerance of `==` and `vanishes` (default 1e-6) |
| `.escapeRadius` | R | `escapes` threshold (default 2) |
| `.stuck`, `.iterations` | Bool, Z | result of the last loop |
| `.lastClick` | C | point of the last click or drag (in the viewer it happened in) |
| `.dragStart` | C | where the mouse button went down (for tools: `on drag`, `on release`) |
| `.mouse` | C | point under the mouse |
| `.time` | R | seconds since the script started (viewers using it are animated) |
| `.pixel` | R | size of a pixel of the viewer, in the plane (in a tool: of the viewer clicked) |
| `.width`, `.height`, `.title` | Z, Z, text | initial window size and title (set them in `@config`) |
| `.VAR`, `NAME.center`, `NAME.radius` | | per viewer, see Sections |

## Tools

A tool runs when you click (and while you drag) in a viewer while the tool is selected. Most tools
belong to one viewer, written `@NAME.tool`; inside it, the viewer's variable is the point you
clicked:

```
@viewer julia z "Julia set" "Ctrl + 2"
...

@julia.tool "Orbit" "O" "timeline"
w := z                      # z is the clicked point
for k in 1..100 do
    w2 := w^2 + .c
    draw line between w and w2
    w = w2
end
draw circle at z with radius 4 px and color .red
```

- The header takes a name, a keyboard shortcut and an icon, all optional. Icons are named like the
  [Material Icons](https://fonts.google.com/icons?icon.set=Material+Icons) (`"timeline"`,
  `"my_location"`, `"brush"`, ...); without one the tool shows the first letter of its name.
- `@tool` (without a viewer) makes a tool available in every viewer; it reads the clicked point
  from `.lastClick`.
- `draw point at P`, `draw circle at P with radius R [px]`, `draw disk at P with radius R [px]`,
  `draw line between A and B`, `draw text "period " + n at P` (texts in quotes and values joined
  with `+`), with optional `with/and color C` and `width W px`, and
  `in VIEWER` to draw in another viewer. Radii are in plane units unless followed by `px`, which
  keeps them that many pixels wide on the screen when you zoom afterwards.
- Each run of a tool replaces the drawings of its previous run, unless the tool sets
  `.keep = true`: then they build up until it runs `erase`. `erase all` clears the drawings of every
  tool, and `erase "Name"` those of the tools named so (for example in a button next to a drawing
  tool: `@tool "Clear marks" "C" "delete" button` / `erase "Mark"`).
- Settings at the top of a tool: `.panning = true` makes dragging pan the view, so the tool only
  runs on clicks; `.hover = true` also runs the tool when the mouse moves without a button;
  `.keep = true` as above.
- `button` at the end of the header (`@tool "Clear" "X" "delete" button`) makes an action: clicking
  its button (or its key) runs it once, at the centre of the view, and it is never selected.

### Events

A tool can handle each event in its own block; the statements outside the blocks run first, every
time:

```
@plane.tool "Move root" "M" "open_with"
on press do                 # the button went down at z
    grabbed = nearest(z, 10 px)        # (a global, so that the next events see it)
end
on drag do                  # the mouse moved to z, button down; .dragStart is where it went down
    move(grabbed, z)
end
on release do               # the button went up at z
    grabbed = 0
end
```

The events are `press`, `drag`, `release`, `click` (a press and release without moving), `hover`
(the mouse moves without a button), `show` (the tool was selected, or the state changed while it
is: a config value, a global set by another tool, the view...; while you pan or zoom, it waits until
the view rests) and `hide` (another tool was selected; for
example `on hide do erase end`). Each block is its own scope: share values between events through
globals. A tool with blocks for mouse events runs only on the events it has blocks for; blocks for
`show` and `hide` alone don't change what the mouse does (the statements outside the blocks still
run on clicks and drags; a tool with nothing outside its blocks, say one that draws `on show`,
ignores the mouse):

```
@plane.tool "Mark" "M" "push_pin"
.keep = true
draw point at z             # on every click and drag
on hide do
    erase                   # when another tool is selected
end
```

Every viewer also has the built-in tools **Pan** (drag to move, wheel to zoom) and **Pick point**
(sets `.VAR` of the viewer you click in and marks it). The mouse wheel always zooms, and dragging
with the right or middle button always pans. Each viewer remembers its own selected tool.

## Config

```
@config
.title = "Julia sets"
.width = 1200
plane.center = -0.5
iterations : Z = 100 in 1..1000      # a slider in the config panel
c := -0.8 + 0.156i                   # an editable complex number

@config "Colors"                     # a group with a title (any number of @config sections)
"The palette is applied to the number of iterations."   # a note, shown as text
palette : Z = 1 in ["Rainbow", "Grays", "Fire"] "Palette"   # a choice: palette is 1, 2 or 3
speed : R = 1 in 0.1..10 "Color speed"                      # a label instead of the name
.maxIterations = 300 in 10..5000 "Iterations"               # sliders work on internals too
```

- Every variable declared in `@config` gets a control in the config panel (Ctrl+T, Cmd+T on a
  Mac), in the order of the script: sliders for `Z` and `R` with a range (logarithmic when the
  range spans more than a factor 100), a list for choices, fields for complex numbers, a check box
  for `Bool`.
- A text in quotes after a declaration (and its range) is its label. A line that is only a text is
  a note.
- Lists of numbers are edited as text, items separated by commas (`1, i, -1, -i`); Enter applies.
- A one-line function defined in `@config` (`define f(z, c) = z^2 + c`) is a formula you can edit
  in the config panel: the field edits the script itself, which runs again as you type (with Live
  on) or when you press Enter.
- The views' centre and radius, and `.maxIterations`, `.escapeRadius` and `.epsilon`, are listed at
  the end unless the script places them itself.
