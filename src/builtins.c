/* builtins.c - intrinsics (implemented natively by interp.c and codegen.c)
 * and the prelude: the public builtin functions, written in the language
 * itself on top of the intrinsics.  Keeping complex/colour functions in the
 * prelude means the CPU interpreter and the GPU code share one definition. */

typedef enum Intrinsic {
    I_NONE,
    /* R -> R (GLSL.std.450 / libm) */
    I_SIN, I_COS, I_TAN, I_ASIN, I_ACOS, I_ATAN, I_SINH, I_COSH, I_TANH, I_ASINH, I_ACOSH, I_ATANH,
    I_EXP, I_LOG, I_SQRT, I_FLOOR, I_CEIL, I_FRACT, I_FABS, I_FSIGN, I_EXP2, I_LOG2, I_TRUNC,
    /* R,R -> R */
    I_ATAN2, I_POW, I_FMIN, I_FMAX, I_FMOD, I_STEP,
    /* R,R,R -> R */
    I_FCLAMP, I_FMIX, I_SMOOTHSTEP,
    /* Z -> Z, Z,Z -> Z */
    I_IABS, I_ISIGN, I_IMIN, I_IMAX, I_IMOD, I_IDIV,
    /* R -> Z */
    I_FLOORI, I_CEILI, I_ROUNDI, I_TRUNCI,
    /* constructors / accessors */
    I_COMPLEX, I_RE, I_IM, I_QUAT,
    /* R -> Bool */
    I_ISNAN, I_ISINF,
    /* generic (checked specially) */
    I_DOT, I_LENGTH, I_APPEND, I_CLEAR,
    I_COUNT
} Intrinsic;

typedef struct IntrinsicInfo {
    const char *name;
    int id;
    int nparams;
    char params[5]; /* 'Z' 'R' 'C' 'H' 'B', '*' generic (a string: NUL-terminated) */
    char ret;
} IntrinsicInfo;

static const IntrinsicInfo intrinsics[] = {
    {"__sin", I_SIN, 1, "R", 'R'}, {"__cos", I_COS, 1, "R", 'R'}, {"__tan", I_TAN, 1, "R", 'R'},
    {"__asin", I_ASIN, 1, "R", 'R'}, {"__acos", I_ACOS, 1, "R", 'R'}, {"__atan", I_ATAN, 1, "R", 'R'},
    {"__sinh", I_SINH, 1, "R", 'R'}, {"__cosh", I_COSH, 1, "R", 'R'}, {"__tanh", I_TANH, 1, "R", 'R'},
    {"__asinh", I_ASINH, 1, "R", 'R'}, {"__acosh", I_ACOSH, 1, "R", 'R'}, {"__atanh", I_ATANH, 1, "R", 'R'},
    {"__exp", I_EXP, 1, "R", 'R'}, {"__log", I_LOG, 1, "R", 'R'}, {"__sqrt", I_SQRT, 1, "R", 'R'},
    {"__floor", I_FLOOR, 1, "R", 'R'}, {"__ceil", I_CEIL, 1, "R", 'R'}, {"__fract", I_FRACT, 1, "R", 'R'},
    {"__fabs", I_FABS, 1, "R", 'R'}, {"__fsign", I_FSIGN, 1, "R", 'R'}, {"__exp2", I_EXP2, 1, "R", 'R'},
    {"__log2", I_LOG2, 1, "R", 'R'}, {"__trunc", I_TRUNC, 1, "R", 'R'},
    {"__atan2", I_ATAN2, 2, "RR", 'R'}, {"__pow", I_POW, 2, "RR", 'R'}, {"__fmin", I_FMIN, 2, "RR", 'R'},
    {"__fmax", I_FMAX, 2, "RR", 'R'}, {"__fmod", I_FMOD, 2, "RR", 'R'}, {"__step", I_STEP, 2, "RR", 'R'},
    {"__fclamp", I_FCLAMP, 3, "RRR", 'R'}, {"__fmix", I_FMIX, 3, "RRR", 'R'},
    {"__smoothstep", I_SMOOTHSTEP, 3, "RRR", 'R'},
    {"__iabs", I_IABS, 1, "Z", 'Z'}, {"__isign", I_ISIGN, 1, "Z", 'Z'}, {"__imin", I_IMIN, 2, "ZZ", 'Z'},
    {"__imax", I_IMAX, 2, "ZZ", 'Z'}, {"__imod", I_IMOD, 2, "ZZ", 'Z'}, {"__idiv", I_IDIV, 2, "ZZ", 'Z'},
    {"__floori", I_FLOORI, 1, "R", 'Z'}, {"__ceili", I_CEILI, 1, "R", 'Z'}, {"__roundi", I_ROUNDI, 1, "R", 'Z'},
    {"__trunci", I_TRUNCI, 1, "R", 'Z'},
    {"complex", I_COMPLEX, 2, "RR", 'C'}, {"re", I_RE, 1, "C", 'R'}, {"im", I_IM, 1, "C", 'R'},
    {"Re", I_RE, 1, "C", 'R'}, {"Im", I_IM, 1, "C", 'R'},
    {"quat", I_QUAT, 4, "RRRR", 'H'},
    {"isnan", I_ISNAN, 1, "R", 'B'}, {"isinf", I_ISINF, 1, "R", 'B'},
    {"dot", I_DOT, 2, "**", '*'}, {"length", I_LENGTH, 1, "*", '*'}, {"append", I_APPEND, 2, "**", '*'},
    {"clear", I_CLEAR, 1, "*", '*'},
};

/* The prelude.  Public names here are what users see as built-in functions.
 * Overloads are chosen by argument type (fewest promotions wins), so
 * sin(2) uses the real version and sin(2 + i) the complex one. */
static const char *prelude_source =
    "define sin(x : R) = __sin(x)\n"
    "define cos(x : R) = __cos(x)\n"
    "define tan(x : R) = __tan(x)\n"
    "define asin(x : R) = __asin(x)\n"
    "define acos(x : R) = __acos(x)\n"
    "define atan(x : R) = __atan(x)\n"
    "define atan(y : R, x : R) = __atan2(y, x)\n"
    "define atan2(y : R, x : R) = __atan2(y, x)\n"
    "define sinh(x : R) = __sinh(x)\n"
    "define cosh(x : R) = __cosh(x)\n"
    "define tanh(x : R) = __tanh(x)\n"
    "define asinh(x : R) = __asinh(x)\n"
    "define acosh(x : R) = __acosh(x)\n"
    "define atanh(x : R) = __atanh(x)\n"
    "define exp(x : R) = __exp(x)\n"
    "define log(x : R) = __log(x)\n"
    "define ln(x : R) = __log(x)\n"
    "define log2(x : R) = __log2(x)\n"
    "define log10(x : R) = __log(x) / 2.302585093\n"
    "define sqrt(x : R) = __sqrt(x)\n"
    "define abs(x : Z) = __iabs(x)\n"
    "define abs(x : R) = __fabs(x)\n"
    "define abs(z : C) = |z|\n"
    "define abs(q : H) = |q|\n"
    "define sign(x : Z) = __isign(x)\n"
    "define sign(x : R) = __fsign(x)\n"
    "define floor(x : R) = __floori(x)\n"
    "define ceil(x : R) = __ceili(x)\n"
    "define round(x : R) = __roundi(x)\n"
    "define trunc(x : R) = __trunci(x)\n"
    "define fract(x : R) = __fract(x)\n"
    "define min(a : Z, b : Z) = __imin(a, b)\n"
    "define max(a : Z, b : Z) = __imax(a, b)\n"
    "define min(a : R, b : R) = __fmin(a, b)\n"
    "define max(a : R, b : R) = __fmax(a, b)\n"
    "define clamp(x : Z, lo : Z, hi : Z) = __imin(__imax(x, lo), hi)\n"
    "define clamp(x : R, lo : R, hi : R) = __fclamp(x, lo, hi)\n"
    "define mix(a : R, b : R, t : R) = __fmix(a, b, t)\n"
    "define mix(a : C, b : C, t : R) = a + (b - a) * t\n"
    "define mix(a : R^3, b : R^3, t : R) = a + (b - a) * t\n"
    "define smoothstep(a : R, b : R, x : R) = __smoothstep(a, b, x)\n"
    "define step(edge : R, x : R) = __step(edge, x)\n"
    /* complex numbers */
    "define arg(z : C) = __atan2(im(z), re(z))\n"
    "define conj(z : C) = complex(re(z), -im(z))\n"
    "define norm(z : C) = re(z) * re(z) + im(z) * im(z)\n"
    "define cis(t : R) = complex(__cos(t), __sin(t))\n"
    "define polar(r : R, t : R) = complex(r * __cos(t), r * __sin(t))\n"
    "define exp(z : C) = __exp(re(z)) * complex(__cos(im(z)), __sin(im(z)))\n"
    "define log(z : C) = complex(__log(|z|), __atan2(im(z), re(z)))\n"
    "define ln(z : C) = log(z)\n"
    "define sqrt(z : C) = w : C where\n"
    "    r : R = |z|\n"
    "    a : R = __sqrt(0.5 * (r + __fabs(re(z))))\n"
    "    if a <= 0 then\n"
    "        w = 0\n"
    "    else if re(z) >= 0 then\n"
    "        w = complex(a, 0.5 * im(z) / a)\n"
    "    else if im(z) >= 0 then\n"
    "        w = complex(0.5 * im(z) / a, a)\n"
    "    else\n"
    "        w = complex(-0.5 * im(z) / a, -a)\n"
    "    end\n"
    "end\n"
    "define sin(z : C) = complex(__sin(re(z)) * __cosh(im(z)), __cos(re(z)) * __sinh(im(z)))\n"
    "define cos(z : C) = complex(__cos(re(z)) * __cosh(im(z)), -__sin(re(z)) * __sinh(im(z)))\n"
    "define tan(z : C) = sin(z) / cos(z)\n"
    "define sinh(z : C) = complex(__sinh(re(z)) * __cos(im(z)), __cosh(re(z)) * __sin(im(z)))\n"
    "define cosh(z : C) = complex(__cosh(re(z)) * __cos(im(z)), __sinh(re(z)) * __sin(im(z)))\n"
    "define tanh(z : C) = sinh(z) / cosh(z)\n"
    "define asin(z : C) = -i * log(i * z + sqrt(1 - z * z))\n"
    "define acos(z : C) = complex(1.570796327, 0) - asin(z)\n"
    "define atan(z : C) = 0.5 * i * (log(1 - i * z) - log(1 + i * z))\n"
    /* powers used by ^ when the exponent is not a small integer constant */
    "define __powr(x : R, y : R) = __pow(x, y)\n"
    "define __powc(z : C, w : C) = p : C where\n"
    "    if re(z) * re(z) + im(z) * im(z) <= 0 then\n"
    "        p = 0\n"
    "    else\n"
    "        p = exp(w * log(z))\n"
    "    end\n"
    "end\n"
    "define __powci(z : C, n : Z) = r : C where\n"
    "    r = 1\n"
    "    b := z\n"
    "    m : Z = __iabs(n)\n"
    "    while m > 0 do\n"
    "        if __imod(m, 2) == 1 then\n"
    "            r = r * b\n"
    "        end\n"
    "        b = b * b\n"
    "        m = __idiv(m, 2)\n"
    "    end\n"
    "    if n < 0 then\n"
    "        r = 1 / r\n"
    "    end\n"
    "end\n"
    "define __powri(x : R, n : Z) = r : R where\n"
    "    r = 1\n"
    "    b : R = x\n"
    "    m : Z = __iabs(n)\n"
    "    while m > 0 do\n"
    "        if __imod(m, 2) == 1 then\n"
    "            r = r * b\n"
    "        end\n"
    "        b = b * b\n"
    "        m = __idiv(m, 2)\n"
    "    end\n"
    "    if n < 0 then\n"
    "        r = 1 / r\n"
    "    end\n"
    "end\n"
    /* quaternions */
    "define conj(q : H) = quat(q[1], -q[2], -q[3], -q[4])\n"
    /* colours (components between 0 and 1; hue in turns) */
    "define rgb(r : R, g : R, b : R) = [r, g, b]\n"
    "define gray(v : R) = [v, v, v]\n"
    "define grey(v : R) = [v, v, v]\n"
    "define hsv(h : R, s : R, v : R) = c : R^3 where\n"
    "    hh : R = 6 * __fract(h)\n"
    "    r : R = __fclamp(__fabs(hh - 3) - 1, 0, 1)\n"
    "    g : R = __fclamp(2 - __fabs(hh - 2), 0, 1)\n"
    "    b : R = __fclamp(2 - __fabs(hh - 4), 0, 1)\n"
    "    c = [v * (1 - s + s * r), v * (1 - s + s * g), v * (1 - s + s * b)]\n"
    "end\n";
