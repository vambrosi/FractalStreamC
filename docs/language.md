# The FractalStreamC language

A FractalStreamC script describes interactive pictures of the complex plane. It has
**viewers** (code that runs for every pixel and produces a color), **tools** (code that runs
when you click in a viewer; it can change variables and draw shapes), **globals** (state shared
by everything) and **config** (settings and variables you can tweak in the GUI).
The examples (Ctrl+O, then Examples) show each feature in a short script, in order.

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

- A file without section headers is one viewer: `@viewer plane z "z-plane" "Ctrl + 1"`. A file
  without any code (empty, or only comments) has no viewer: the app shows the script alone.
- Code before the first header is treated as `@global`.
- Deleting a viewer's section closes its view in the app at once, even while the script has errors;
  the other views keep their places, and an undo brings it back where it was. A viewer renamed in
  its header keeps its place on the screen.
- `@global` and `@config` run once, in file order, when the script is loaded. The variables they
  declare are the program **state**. Config variables also get a control in the config panel.
- `@prepare` runs (on the CPU) after them, and again whenever the state changes (a config value, a
  tool, a picked point, the view; not the mouse), before the pictures are drawn. Use it
  for values the viewers need that are computed once from the state instead of at every pixel,
  such as a fixed point that depends on a parameter. It can change globals; keep it quick, or
  guard it with an `if` that checks whether its inputs changed. If it reads `.time`, it runs at
  every frame while the time runs: setting `NAME.center` and `NAME.radius` from `.time` moves a
  view by itself, in the app and in videos (a zoom: `examples/4 - More/4 - Moving views.fstream`;
  see *Time*).
- In a viewer, `VAR` is the point of the current pixel (complex). The viewer sets `color`
  (it starts black). `return` ends the viewer early.
- Every viewer `NAME` with variable `VAR` adds three state variables: `.VAR` (the last point picked
  in it, see *Pick point* below), `NAME.center` and `NAME.radius` (the view: `NAME.radius` is the
  radius of the largest disk around the center that fits in the view; defaults 0 and 2).
- Adding `projective` at the end of a viewer header shows the whole **Riemann sphere**:
  `@viewer sphere z "Riemann sphere" "Ctrl + 3" projective`. The variable is a complex
  number (it can be huge near infinity), or, written `z : P`, a point of the sphere exact at
  infinity: `@viewer sphere z : P "Riemann sphere" projective` (see *Points of the Riemann sphere*).
  Dragging rotates the sphere, the wheel zooms around
  the point under the mouse (a hyperbolic Möbius map fixing it and its antipode). `NAME.center` and
  `NAME.radius` set the view (the disk of that radius around the center, as in a flat view): at the
  start, and whenever `@prepare` or a tool changes them. The current view is the Möbius map `NAME.mobius`.
  The view is **conformal**: the sphere is seen from a point of it (stereographic projection), so
  the picture is the plane through a Möbius map, angles are kept and circles stay circles.
  `NAME.conformal = false` (also a check box in the config panel) looks from the sphere's center
  instead, like a camera inside it (straight lines are great circles; shapes stretch toward the
  edges).
- Shortcuts are written like `"Ctrl + 1"`, `"Shift + P"`, `"O"`. On macOS, Ctrl means Cmd.
  Viewers without a shortcut get Ctrl + 1, Ctrl + 2, ... in order.

## Values and types

| type | meaning | literals / constructors |
|---|---|---|
| `Z` (`Integer`) | 32-bit integers | `42` |
| `R` (`Real`) | real numbers (32-bit floats) | `2.5`, `1e-3`, `pi`, `e`, `tau` |
| `C` (`Complex`) | complex numbers | `3i`, `2 + 0.5i`, `complex(x, y)`, `cis(t)`, `polar(r, t)` |
| `H` (`Quaternion`) | quaternions | `quat(a, b, c, d)`, `.j`, `.k` |
| `P` (`Projective`) | points of the Riemann sphere | `[a : b]`, `∞` (or `.infinity`), any complex number |
| `Bool` (`Boolean`) | `true` / `false` | comparisons, `and`, `or`, `not` |
| `R^3`, `C^2`, `Z^5`, ... | fixed-size vectors | `[1, 2, 3]` |
| `Color` | same as `R^3` (red, green, blue in 0..1) | `rgb(r, g, b)`, `hsv(h, s, v)`, `gray(v)`, `.red` |
| `List of C` (etc.) | list of up to 64 numbers (`List of C up to 1000`: up to 1000) | `[]`, `[1, i]`, `append(L, x)` |

`i` is the imaginary unit and can't be used as a variable name. A number has the type it is written
with: `2` is `Z`, `2.5` is `R` (so are `pi`, `e`, `tau`), `2i` and `2 + 0.5i` are `C`.

**Promotion.** Numbers convert automatically along `Z -> R -> C -> H` (and elementwise for
vectors), never backwards: use `re`, `im`, `|z|`, `floor`, `ceil`, `round` to go down.
Functions follow the type of their argument: `sqrt(-4)` and `log(-1)` are real functions of a real
number, so they give NaN (not a number), while `sqrt(-4 + 0i)` is `2i`. A variable meant to be complex
from the start is declared so: `x : C = -4`.

**Precision.** Reals are 32-bit floats (GPUs on macOS have no doubles). A flat view zoomed in until a
pixel is smaller than about a millionth of the numbers in view gets blocky; with `.doublePrecision`
on (a check box in the config panel, or `.doublePrecision = true` in `@config`; off by default), it
is then computed again in double-single precision (pairs of floats, about 14 digits) — slower, but
sharp down to a radius of about `1e-12` times the size of the numbers. While it's off, such a view
shows a note that turns it on with a click. The coordinates at the bottom of the view say
"(double precision)" while it's used. Square roots and the four operations get the extra digits; other
functions (`exp`, `log`, `sin`, ...) stay in single precision, which is what colors need. Tools,
`@prepare` and `@global` run on the CPU in single precision. A view's center written as numbers in
`@config` (`mandel.center = -0.743643887037151 + 0.131825904205330i`, as Copy location writes it)
keeps all its digits, so a script can open at a deep place.

### Points of the Riemann sphere (`P`)

A value of type `P` is a point `[a : b]` of the sphere: the complex number a/b, or `∞` when b = 0.
Complex numbers become points by themselves (`p : P = 2i` is `[2i : 1]`). Arithmetic, powers,
`==`, `escapes`, `vanishes` and `|p|` work as for complex numbers, and stay exact at infinity:

```
define f(z : P) = (z^2 + 1)/(z^2 - 1)
f(∞)                      # 1, not [0 : 0]
1/[0 : 1]                 # ∞
p == q                    # closer than .epsilon in the chordal distance
p escapes                 # |p| > .escapeRadius, as for complex numbers; ∞ escapes
p[1], p[2]                # the coordinates a and b (complex), of one lift
complex(p)                # a/b (a huge number near ∞)
chordal(p, q)             # the chordal distance: 0 to 1 (0 and ∞ are at 1)
L : List of P = [0, ∞, 1 + i]
```

A formula of a point `z` (sums, products, quotients and integer powers of `z` and of numbers) is
computed with the coordinates of `z`, the common powers of `z` (and of its coordinates) canceled, so
a rational map written as a formula is right everywhere, also where `z` or its value is `∞`. Two
things lose that:

- a value kept in a variable along the way: `w := z^2` then `w / z` is `[0 : 0]` at infinity, where
  `z^2 / z` is `∞`. Write maps as one formula, or as a function of `z`;
- terms that cancel each other: `z - z^2/(z + 1)` is `z/(z + 1)`, which is 1 at infinity, but
  written this way its leading terms cancel and it gives `[0 : 0]` there (so does
  `(z + 1)^2 - z^2`). Simplify such formulas first.

Transcendental functions (`exp`, `sin`, ...) take complex numbers: use `complex(p)`. `derivative`,
`root` and `critical` work on points too (see below).

## Variables

```
x : R = 2.5          # declaration with a type
n : Z                # declared, starts at 0
z := 1 + i           # declaration without a type: see below
w := 0
w = w^2 + c          # assignment, to a variable declared before
w += 1               # also -=, *=, /=
v[2] = 0             # vectors, lists, and the parts of C (z[1] = re, z[2] = im) and H
(a, b) := f(x)       # unpack a function's results
(x, y) = (x^2 + c - a y, x)   # parallel assignment: every value is computed before any is stored
```

- **Declare, then assign.** `x := ...` (or `x : T = ...`) declares a variable; `x = ...` assigns
  one that is already declared. Assigning a name that isn't declared is an error (with a "did you
  mean ...?" when it looks like a typo), so a misspelled name can't quietly become a new variable.
- **A variable declared without a type takes the widest type it is given:** first the type of its
  value, then wider if the script assigns it wider values. With `w := 0` and then
  `w = w^2 + c` (`c` complex), `w` is complex; with `t := 0` and `t = t + 0.1`, `t` is real;
  `n := 0` stays an integer if it only counts. `r := |z|` is real, `(n, ok) := f(x)` keeps the
  types of `f`'s results, and vectors are at least real (`v := [1, 2]` is `R^2`). Hover over a
  name in the editor to see its type.
- Give the type when it matters from the start: `c : C = 0` for a config value you will set to
  complex numbers (`c := 0` makes a field for integers), `x : R = 1` for a real number.
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

A statement ends at the end of its line, unless the line ends with an operator or a comma
(`+ - * / ^ = := == < and or not mod ->` ...), or with `...` (or `…`), or a parenthesis or
bracket is still open: then it goes on on the next line (blank lines and comment lines in
between don't end it).

```
m : R = .iterations + 1 -
        log(log(|z|^2) / (2 log(.escapeRadius))) / log(2)
color = if use_arenberg then palette("arenberg", m) ...
        else rainbow(m)
"A long note in the config panel " +          # texts in quotes joined with + are one text
"goes on on the next line."
```

`while` and `do` loops stop after `.maxIterations` iterations; `for` loops run their whole range
(up to a million steps). After a loop, `.stuck` tells whether it was stopped by its limit, and
`.iterations` how many times its body ran.

## Functions

```
define f(z) = z^2 + c                    # one expression
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

**Parameters without a type take the type of the argument.** `f(2)` computes with an integer,
`f(0.5)` with a real number and `f(z)` with a complex number: each list of argument types gets its
own copy of `f`, made when the script is checked, so one definition serves every type it makes
sense for (`sq(x) = x^2` squares integers, reals, complex numbers and vectors). Inside, such a
parameter can still get wider, like a variable declared without a type. Results without a type
take the widest type assigned to them. A function nothing calls yet is checked as if called with
complex numbers (or real numbers, or integers: the first that works), so typos show up at once.
A parameter with a type converts its argument (`g(x : R)` turns `g(2)` into `g(2.0)`).
A result with the name of a parameter is that parameter, starting from the argument: with
`define orbit(z, c) = z where` and a loop doing `z = z^2 + c`, the result is the last `z`.

Functions see the global state (and may change it), their parameters, their results and their own
variables; not the variables of the section that calls them. Functions with the same name but
different parameter types are allowed (the closest match is used; when one with types and one
without fit equally well, the one with types is used). Recursion is not supported.

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

**On the sphere.** With a point of `P` after `at` or `with seed`, the variable is a point too, and
`f` may be a map to the sphere. So is it when the expression is a point (a map to the sphere), even
if the point after `at` is written as a complex number, and then `root` and `critical` give points:

```
define f(z : P) = (z^2 + a)/(z^2 - 1)
d := derivative f at w                   # in the natural charts of w and of f(w)
c := critical t -> f(t) with seed ∞      # a critical point of f, a point (0 and ∞ too)
r := root t -> f(t) with seed [1 : 0.1]  # a point where f is 0
mu := (derivative f at x) * (derivative f at f(x))   # the multiplier of the 2-cycle x, f(x)
```

Derivatives use the *natural chart* of each point: the coordinate z near points with |z| ≤ 1 and
1/z near the others (so near ∞, s = 1/z). Their product along a cycle is its multiplier, whatever the
charts. Newton's method moves the point in its chart and changes chart when it leaves the unit disk;
critical points are where the derivative of the map vanishes in any chart, so they are found at poles
and at ∞ too. (Only maps written as formulas, or as one-line functions of points, can be
differentiated; see `examples/3 - The sphere/2 - Attracting cycles.fstream`.)

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
  neighbor that is already done, moving `c` from the neighbor's point to its own in small
  steps. The fill grows outward from `P` and goes around pixels where Newton's method fails
  (they pass nothing on); pixels it can't reach at all start from the seed.
- A solution with several branches (like the critical points in SaddleDrop) is followed
  continuously; where the fill comes back around an obstacle with another branch, the picture
  shows a line. See [SaddleDrop](../examples/5%20-%20Gallery/0%20-%20SaddleDrop.fstream) in the examples.
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
| colors | `rgb(r, g, b)`, `hsv(h, s, v)` (hue in turns: 0 red, 1/3 green, 2/3 blue), `gray(v)`, `mix(a, b, t)` |
| palettes | `rainbow(t)`, `palette("name", t)` with the maps `rainbow arenberg romao bamo broco corko viko` (cyclic) and `ice fire wheat rose forest ocean winter spring summer fall` (there and back); all repeat with period 1 in `t` |
| from the first FractalStream | `cycle(t, a, b)` (b, then a at t = 1/2, back to b), `blend(t, a, b)` (= `mix(b, a, t)`), `dark(c)`, `light(c)`, `invert(c)` |
| points of the sphere | `complex(p)`, `conj(p)`, `chordal(p, q)`, `p[1]`, `p[2]` |

A script may define its own version of any of these (a function with the same name, and the same
parameter types or none): its own is used instead. Only `re`, `im`, `complex(x, y)`, `quat`,
`isnan`, `isinf`, `dot`, `length`, `append` and `clear` can't be replaced.

Constants: `pi`, `e`, `tau` (can be shadowed by your own variables), colors `.red .green .blue
.black .white .gray .yellow .cyan .magenta .orange .purple .pink .brown .violet`, quaternion units
`.j .k`.

Most of these functions are written in the language itself: the reference in the app (F2) ends
with their definitions, and `fractalstreamc --builtins` prints them.

## Internals (names starting with a dot)

| name | type | meaning |
|---|---|---|
| `.maxIterations` | Z | iteration limit of `while` and `do` loops (default 1000) |
| `.epsilon` | R | tolerance of `==` and `vanishes` (default 1e-6) |
| `.escapeRadius` | R | `escapes` threshold (default 2) |
| `.doublePrecision` | Bool | deep zooms in double precision (default false; see *Precision*) |
| `.antialiasing` | Z | samples per pixel of a view at rest, 1 to 16 (default 8; 1: off): once nothing moves, the picture is drawn again with the samples moved inside the pixels, and averaged |
| `.stuck`, `.iterations` | Bool, Z | result of the last loop |
| `.lastClick` | C | point of the last click or drag (in the viewer it happened in) |
| `.dragStart` | C | where the mouse button went down (for tools: `on drag`, `on release`) |
| `.mouse` | C | point under the mouse |
| `.time` | R | the time in seconds, from 0 when the script starts; it stops while paused (viewers using it are animated, an `@prepare` using it runs at every frame); in a video, the time of the frame. Only tools can set it (see *Time*) |
| `.playing` | Bool | true while the time runs, and in videos; false while it is paused, and in pictures (`--render`) |
| `.duration` | R | the length of the animation in seconds (default 0: none): `.time` loops over it, and videos last that long |
| `.pixel` | R | size of a pixel of the viewer, in the plane (in a tool: of the viewer clicked) |
| `.width`, `.height`, `.title` | Z, Z, text | initial window size and title (set them in `@config`) |
| `.VAR`, `NAME.center`, `NAME.radius` | | per viewer, see Sections |

### Time

`.time` runs from 0 when the script starts. A viewer that reads it is drawn again at every frame,
and an `@prepare` that reads it runs at every frame (it can move the views). Space, or the bar under
the views, pauses and plays the time; the bar and the Left and Right keys move it (see the app's
docs). Running the script again keeps the time, so a paused moment stays while you edit.

```
@config
seconds := 20 in 4..120 "Seconds of the movie"

@prepare
.duration = seconds          # .time loops from 0 to seconds

@viewer julia z "Julia set"
c := 0.7885 cis(2 pi .time / seconds)
limit := if .playing then 100 else 1000   # quick while it runs
...
```

- `.duration` gives the animation a length: `.time` loops from 0 to it, the time bar can scrub
  through it, and videos last that long. It is a state variable: set it in `@config`, `@global`
  or (when it depends on the config) `@prepare`.
- `.playing` is true while the time runs: a viewer can draw a quick picture then, and a finer
  one when paused. Videos play (it is true in them); pictures don't.
- A tool can set `.time`, and nothing else can: the clock goes there, still playing or paused.

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
  its button (or its key) runs it once, at the center of the view, and it is never selected.

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

Every viewer also has the built-in tools **Pan** (drag to move, wheel to zoom), **Pick point**
(sets `.VAR` of the viewer you click in and marks it) and **Inspect** (key `?`: runs the viewer's
code for the point you click, on the CPU, and lists its variables at the end — and what it printed —
in the Info pane; drag to follow the mouse). The mouse wheel always zooms, and dragging with the
right or middle button always pans. Each viewer remembers its own selected tool.

### Printing

`print` writes a line in the **Info** pane (the `i` button in the right bar, Ctrl+I / Cmd+I),
with the same texts as `draw text` — texts in quotes and values joined with `+`:

```
print "period " + n + ", multiplier " + mu + ", cycle " + L     # L a list: [z1, z2, ...]
```

- In a tool, each run replaces the lines of its previous run, like its drawings; with
  `.keep = true` they build up until `erase` (which also clears them). The pane opens the first
  time a click makes a tool print.
- In `@prepare` they are replaced at each run; in `@global` and `@config` they show what happened
  when the script started.
- In a viewer, `print` does nothing in the picture: the Inspect tool shows what it prints for the
  point you click.
- The pane has a section for each tool (and for Inspect, `@prepare` and the script), each with
  buttons to copy and clear it.

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

- The config panel (like the script and the Info pane) floats over the views, translucent, so the
  pictures keep the whole window; the layers button at the bottom of the right bar puts the panes
  beside the views instead.
- Every variable declared in `@config` gets a control in the config panel (Ctrl+T, Cmd+T on a
  Mac), in the order of the script: sliders for `Z` and `R` with a range (logarithmic when the
  range spans more than a factor 100), a list for choices, fields for complex numbers and points of
  the sphere, a check box for `Bool`. A range written with real numbers makes a real variable
  (`speed := 1 in 0.1..10` is `R`).
- A text in quotes after a declaration (and its range) is its label. A line that is only a text is
  a note.
- Lists of numbers are edited as text, items separated by commas (`1, i, -1, -i`), and points of
  the sphere as a complex number or `inf`; Enter, or leaving the field, applies (Escape doesn't).
- A one-line function defined in `@config` (`define f(z, c) = z^2 + c`) is a formula you can edit
  in the config panel: the field edits the script itself, which runs again as you type (with Live
  on) or when you press Enter.
- The views' center and radius, `.doublePrecision`, `.antialiasing`, `.maxIterations`,
  `.escapeRadius` and `.epsilon` are listed at the end unless the script places them itself.
- A dot marks a value that is no longer the script's (changed in the panel or by a tool); its
  label's tooltip says what the script has, and a click on the dot goes back to it. Values changed this way stay when the script runs
  again, unless the script itself now gives them another value. The two buttons at the top of
  the panel set them back as the script has them, or write them into the script (with the places
  of the views), so that it opens like this: numbers in the script are replaced, formulas are left
  alone.
