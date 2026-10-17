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
    /* continuation (autodiff.c, flood.c): never written by users */
    I_FLOOD_SEED, I_FLOOD_FROM, I_FLOOD_OUT,
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
    "end\n"
    /* colour helpers and palettes of the first FractalStream (a script may define its own instead) */
    "define blend(t : R, a : R^3, b : R^3) = a * t + b * (1 - t)\n"
    "define cycle(t : R, a : R^3, b : R^3) = c : R^3 where\n"
    "    u : R = 2 * __fract(t)\n"
    "    if u > 1 then\n"
    "        u = 2 - u\n"
    "    end\n"
    "    c = a * u + b * (1 - u)\n"
    "end\n"
    "define dark(c : R^3) = c * (2.0 / 3.0)\n"
    "define light(c : R^3) = c + ([1, 1, 1] - c) / 3\n"
    "define invert(c : R^3) = [1, 1, 1] - c\n"
    "define __wheel(t : R, c1 : R^3, c2 : R^3, c3 : R^3, c4 : R^3, c5 : R^3, c6 : R^3, c7 : R^3, c8 : R^3) = c : R^3 where\n"
    "    s : R = 8 * __fract(t)\n"
    "    if s < 4 then\n"
    "        if s < 2 then\n"
    "            if s < 1 then\n"
    "                c = c1 + (c2 - c1) * s\n"
    "            else\n"
    "                c = c2 + (c3 - c2) * (s - 1)\n"
    "            end\n"
    "        else if s < 3 then\n"
    "            c = c3 + (c4 - c3) * (s - 2)\n"
    "        else\n"
    "            c = c4 + (c5 - c4) * (s - 3)\n"
    "        end\n"
    "    else if s < 6 then\n"
    "        if s < 5 then\n"
    "            c = c5 + (c6 - c5) * (s - 4)\n"
    "        else\n"
    "            c = c6 + (c7 - c6) * (s - 5)\n"
    "        end\n"
    "    else if s < 7 then\n"
    "        c = c7 + (c8 - c7) * (s - 6)\n"
    "    else\n"
    "        c = c8 + (c1 - c8) * (s - 7)\n"
    "    end\n"
    "end\n"
    "define rainbow(t : R) = __wheel(t, [1, 0, 0], [1, 0.3922, 0], [1, 1, 0], [0, 0.7059, 0], [0, 0.502, 0.502], [0, 0, 1], [0.502, 0, 0.502], [1, 0, 0.6275])\n"
    "define __palette_arenberg(t : R) = __wheel(t, [1, 0.3137, 0.3176], [1, 0.6824, 0], [0.5961, 0.7451, 0], [0, 0.7176, 0.2902], [0, 0.9294, 0.9098], [0, 0.6745, 1], [0.7216, 0.6314, 1], [1, 0.5255, 0.8863])\n"
    "define __palette_romao(t : R) = __wheel(t, [0.7882, 0.8549, 0.6627], [0.7686, 0.6392, 0.2824], [0.6431, 0.4039, 0.1686], [0.5373, 0.251, 0.2078], [0.4549, 0.2235, 0.3451], [0.3647, 0.3294, 0.549], [0.3176, 0.5294, 0.7255], [0.5333, 0.7725, 0.7882])\n"
    "define __palette_bamo(t : R) = __wheel(t, [0.8157, 0.7843, 0.7412], [0.8314, 0.6353, 0.7686], [0.698, 0.4314, 0.6275], [0.4627, 0.2314, 0.4], [0.3059, 0.1765, 0.2471], [0.3098, 0.2706, 0.2314], [0.4118, 0.4314, 0.2941], [0.6941, 0.7294, 0.5882])\n"
    "define __palette_broco(t : R) = __wheel(t, [0.7882, 0.7961, 0.702], [0.5451, 0.6353, 0.7451], [0.349, 0.451, 0.6078], [0.2235, 0.2549, 0.3922], [0.2235, 0.1804, 0.2157], [0.2706, 0.2471, 0.1608], [0.4275, 0.4118, 0.2353], [0.651, 0.6471, 0.4627])\n"
    "define __palette_corko(t : R) = __wheel(t, [0.6471, 0.7686, 0.6784], [0.5255, 0.6157, 0.7255], [0.3373, 0.4353, 0.5804], [0.251, 0.2667, 0.3647], [0.251, 0.2431, 0.2235], [0.2706, 0.3098, 0.1686], [0.3098, 0.451, 0.2275], [0.4588, 0.6431, 0.4353])\n"
    "define __palette_viko(t : R) = __wheel(t, [0.8235, 0.702, 0.6314], [0.4667, 0.6157, 0.7333], [0.2392, 0.4078, 0.5922], [0.2353, 0.2078, 0.4039], [0.3137, 0.09804, 0.2314], [0.4118, 0.09412, 0.1333], [0.5882, 0.2431, 0.1373], [0.7961, 0.5294, 0.3882])\n"
    "define __palette_ice(t : R) = __wheel(t, [0.1176, 0.7961, 1], [0.3725, 0.8471, 0.9843], [0.5373, 0.8902, 0.9765], [0.6784, 0.9373, 0.9765], [0.8118, 0.9765, 0.9882], [0.6784, 0.9373, 0.9765], [0.5373, 0.8902, 0.9765], [0.3725, 0.8471, 0.9843])\n"
    "define __palette_fire(t : R) = __wheel(t, [1, 0.2392, 0.2392], [1, 0.4824, 0.2235], [1, 0.6706, 0.2784], [1, 0.8314, 0.4039], [1, 0.9804, 0.5804], [1, 0.8314, 0.4039], [1, 0.6706, 0.2784], [1, 0.4824, 0.2235])\n"
    "define __palette_wheat(t : R) = __wheel(t, [0, 0.702, 1], [0.4, 0.6863, 0.898], [0.5412, 0.6745, 0.7961], [0.9529, 0.8431, 0.6431], [0.9294, 0.949, 0], [0.9529, 0.8431, 0.6431], [0.5412, 0.6745, 0.7961], [0.4, 0.6863, 0.898])\n"
    "define __palette_rose(t : R) = __wheel(t, [1, 0, 0.6], [1, 0.3451, 0.3843], [0.9686, 0.5725, 0.2549], [0.8353, 0.749, 0.302], [0.702, 0.8863, 0.5137], [0.8353, 0.749, 0.302], [0.9686, 0.5725, 0.2549], [1, 0.3451, 0.3843])\n"
    "define __palette_forest(t : R) = __wheel(t, [0.04706, 0.2863, 0], [0.2588, 0.3412, 0.07059], [0.4039, 0.3922, 0.1725], [0.5255, 0.4471, 0.2902], [0.6235, 0.5137, 0.4196], [0.5255, 0.4471, 0.2902], [0.4039, 0.3922, 0.1725], [0.2588, 0.3412, 0.07059])\n"
    "define __palette_ocean(t : R) = __wheel(t, [0.03922, 0, 0.2863], [0, 0.2039, 0.4157], [0.01569, 0.3686, 0.4902], [0.3294, 0.5294, 0.549], [0.5961, 0.6784, 0.6549], [0.3294, 0.5294, 0.549], [0.01569, 0.3686, 0.4902], [0, 0.2039, 0.4157])\n"
    "define __palette_winter(t : R) = __wheel(t, [0.9686, 0.9804, 1], [0.7647, 0.7961, 0.8745], [0.5804, 0.6157, 0.7529], [0.4196, 0.4431, 0.6275], [0.2745, 0.2745, 0.502], [0.4196, 0.4431, 0.6275], [0.5804, 0.6157, 0.7529], [0.7647, 0.7961, 0.8745])\n"
    "define __palette_spring(t : R) = __wheel(t, [0.4039, 0.8745, 0.5098], [0.5333, 0.8118, 0.3412], [0.6431, 0.7373, 0.1922], [0.7412, 0.6549, 0.0549], [0.8275, 0.5569, 0.03137], [0.7412, 0.6549, 0.0549], [0.6431, 0.7373, 0.1922], [0.5333, 0.8118, 0.3412])\n"
    "define __palette_summer(t : R) = __wheel(t, [0.1882, 0.3608, 0.8078], [0, 0.5294, 0.7059], [0, 0.6314, 0.6941], [0, 0.7569, 0.5843], [0.1373, 0.8902, 0], [0, 0.7569, 0.5843], [0, 0.6314, 0.6941], [0, 0.5294, 0.7059])\n"
    "define __palette_fall(t : R) = __wheel(t, [0.502, 0.2157, 0.01176], [0.6196, 0.3882, 0.102], [0.7294, 0.5608, 0.2157], [0.8314, 0.7412, 0.3529], [0.9216, 0.9255, 0.5137], [0.8314, 0.7412, 0.3529], [0.7294, 0.5608, 0.2157], [0.6196, 0.3882, 0.102])\n"
    "define __palette_rainbow(t : R) = rainbow(t)\n";
