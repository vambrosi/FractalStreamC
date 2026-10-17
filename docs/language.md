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
@viewer NAME VAR ["Display name"] ["Shortcut"]
@tool ["Display name"] ["Shortcut"]
```

- A file without section headers is one viewer: `@viewer plane z "z-plane" "Ctrl + 1"`.
- Code before the first header is treated as `@global`.
- `@global` and `@config` run once, in file order, when the script is loaded. The variables they
  declare are the program **state**. Config variables also get a control in the config panel.
- In a viewer, `VAR` is the point of the current pixel (complex). The viewer sets `color`
  (it starts black). `return` ends the viewer early.
- Every viewer `NAME` with variable `VAR` adds three state variables: `.VAR` (the last point picked
  in it, see *Pick point* below), `NAME.center` and `NAME.radius` (the view: `NAME.radius` is the
  radius of the largest disk around the centre that fits in the view; defaults 0 and 2).
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
| `List of C` (etc.) | list of up to 64 numbers | `[]`, `[1, i]`, `append(L, x)` |

`i` is the imaginary unit and can't be used as a variable name.

**Promotion.** Numbers convert automatically along `Z -> R -> C -> H` (and elementwise for
vectors), never backwards: use `re`, `im`, `|z|`, `floor`, `ceil`, `round` to go down.

**Precision.** Reals are 32-bit floats everywhere (GPUs on macOS have no doubles), so deep zooms
lose detail below about `radius = 1e-5`.

## Variables

```
x : R = 2.5          # declaration with a type
n : Z                # declared, starts at 0
z := 1 + i           # declaration with the type of the value
w = z^2              # assignment; declares w if no variable named w exists yet
w += 1               # also -=, *=, /=
v[2] = 0             # vectors, lists, and the parts of C (z[1] = re, z[2] = im) and H
(a, b) := f(x)       # unpack a function's results
```

- **Untyped numbers are complex.** `n := 0` makes `n` complex, so `for k in 1..n` is an error;
  write `n : Z = 0`. Untyped vectors are at least real (`v := [1, 2]` is `R^2`).
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
- Out-of-range indices are clamped to the first/last entry (indices start at 1).

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

Every loop stops after `.maxIterations` iterations. After a loop, `.stuck` tells whether it was
stopped by that limit, and `.iterations` how many times its body ran.

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

Derivatives are exact (computed symbolically when the script is compiled) for `+ - * /`, powers,
`exp log sqrt sin cos tan sinh cosh tanh asin acos atan` and one-line functions written with
`define`. Other variables in the expression are constants for the derivative. After `root` and
`critical`, `.stuck` is true if Newton's method did not converge within `.maxIterations` steps and
`.iterations` is the number of steps. Non-holomorphic functions (`re`, `im`, `conj`, `|z|`, ...)
can't be differentiated.

## Built-in functions

| | |
|---|---|
| real and complex | `sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh exp log ln sqrt` |
| real only | `atan2(y, x)` (also `atan(y, x)`), `log2 log10 floor ceil round trunc fract sign min max clamp mix smoothstep step isnan isinf` |
| complex | `re im arg conj norm abs complex(x, y) cis(t) polar(r, t)` |
| quaternions | `quat(a, b, c, d)`, `conj`, `abs`, `q[1]` ... `q[4]` |
| vectors / lists | `dot(u, v)`, `length(v)`, `append(L, x)`, `clear(L)` |
| colours | `rgb(r, g, b)`, `hsv(h, s, v)` (hue in turns: 0 red, 1/3 green, 2/3 blue), `gray(v)`, `mix(a, b, t)` |

Constants: `pi`, `e`, `tau` (can be shadowed by your own variables), colours `.red .green .blue
.black .white .gray .yellow .cyan .magenta .orange .purple .pink .brown`, quaternion units
`.j .k`.

## Internals (names starting with a dot)

| name | type | meaning |
|---|---|---|
| `.maxIterations` | Z | iteration limit of every loop (default 1000) |
| `.epsilon` | R | tolerance of `==` and `vanishes` (default 1e-6) |
| `.escapeRadius` | R | `escapes` threshold (default 2) |
| `.stuck`, `.iterations` | Bool, Z | result of the last loop |
| `.lastClick` | C | point of the last click or drag (in the viewer it happened in) |
| `.mouse` | C | point under the mouse |
| `.time` | R | seconds since the script started (viewers using it are animated) |
| `.width`, `.height`, `.title` | Z, Z, text | initial window size and title (set them in `@config`) |
| `.VAR`, `NAME.center`, `NAME.radius` | | per viewer, see Sections |

## Tools

A tool runs when you click (and while you drag) in a viewer while the tool is selected:

```
@tool "Orbit" "O"
w := .lastClick
for k in 1..100 do
    w2 := w^2 + .c
    draw line between w and w2
    w = w2
end
draw circle at .lastClick with radius 4 px and color .red
```

- `draw point at P`, `draw circle at P with radius R [px]`, `draw disk at P with radius R [px]`,
  `draw line between A and B`, with optional `with/and color C` and `width W px`, and
  `in VIEWER` to draw in another viewer. Radii are in plane units unless followed by `px`.
- Each run of a tool replaces the drawings of its previous run.
- Settings at the top of a tool: `.panning = true` makes dragging pan the view, so the tool only
  runs on clicks; `.hover = true` also runs the tool when the mouse moves without a button.

Every script also has the built-in tools **Pan** (drag to move, wheel to zoom) and **Pick point**
(sets `.VAR` of the viewer you click in and marks it). The mouse wheel always zooms, and dragging
with the right or middle button always pans.

## Config

```
@config
.title = "Julia sets"
.width = 1200
.maxIterations = 300
plane.center = -0.5
iterations : Z = 100 in 1..1000      # a slider in the config panel
c := -0.8 + 0.156i                   # an editable complex number
```
