/* ast.h - data structures of the FractalStream language front-end:
 * tokens, types, AST, symbols, functions, sections, programs, diagnostics.
 *
 * Pipeline: lex.c (text -> tokens), parse.c (tokens -> AST), check.c (name
 * resolution, types, implicit conversions, layout), then interp.c (CPU) or
 * codegen.c (SPIR-V).  Everything a program allocates lives in its arena. */
#ifndef FS_AST_H
#define FS_AST_H

/* ---- diagnostics ----------------------------------------------------- */
typedef enum { DIAG_ERROR, DIAG_WARNING, DIAG_NOTE } DiagSeverity;
typedef struct Diag {
    DiagSeverity severity;
    int line, col, end_col; /* 1-based; end_col exclusive (0 = unknown) */
    char *msg;
    bool noted;             /* (check.c: says which instance of a generic function it is about) */
} Diag;
typedef VEC(Diag) DiagVec;

/* ---- tokens ---------------------------------------------------------- */
typedef enum TokKind {
    TK_EOF, TK_NEWLINE, TK_IDENT, TK_INTERNAL /* .name */, TK_INT, TK_REAL, TK_IMAG, TK_STRING,
    TK_SECTION, /* @word */
    TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET, TK_COMMA, TK_COLON, TK_SEMI, TK_BAR, TK_DOTDOT,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_CARET,
    TK_ASSIGN, TK_DECLARE /* := */, TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE, TK_ARROW,
    /* keywords */
    TK_DEFINE, TK_WHERE, TK_END, TK_FOR, TK_IN, TK_BY, TK_DO, TK_WHILE, TK_IF, TK_THEN, TK_ELSE,
    TK_RETURN, TK_BREAK, TK_CONTINUE, TK_AND, TK_OR, TK_NOT, TK_TRUE, TK_FALSE, TK_ESCAPES,
    TK_VANISHES, TK_DRAW, TK_AT, TK_WITH, TK_BETWEEN, TK_PX, TK_MOD, TK_OF,
    TK_COUNT
} TokKind;

typedef struct Token {
    TokKind kind;
    int line, col, len;   /* position of the token text in the source */
    bool space_before;    /* whitespace (or line start) precedes the token */
    const char *text;     /* points into the source */
    double num;           /* TK_INT / TK_REAL / TK_IMAG value */
    char *str;            /* TK_STRING contents, TK_IDENT/TK_INTERNAL/TK_SECTION name (arena) */
} Token;
typedef VEC(Token) TokenVec;

const char *tok_kind_name(TokKind k);

/* ---- types ------------------------------------------------------------
 * Types are interned: compare with ==.  Layout is in 32-bit words and
 * follows std430 (the GPU storage-buffer layout), so the interpreter's
 * frames and the global state buffer share one representation:
 *   Bool -> u32 0/1, Z -> i32, R -> f32, C -> 2 x f32 (align 2),
 *   H -> 4 x f32 (align 4), R^n etc. -> array, List -> {len, data[cap]}. */
typedef enum TypeKind {
    T_ERROR, T_VOID, T_BOOL, T_INT, T_REAL, T_COMPLEX, T_QUAT, T_VEC, T_LIST, T_TUPLE, T_STRING
} TypeKind;

typedef struct Type Type;
struct Type {
    TypeKind kind;
    int n;           /* T_VEC: length, T_LIST: capacity, T_TUPLE: item count */
    Type *elem;      /* T_VEC / T_LIST element (a scalar number type or Bool) */
    Type **items;    /* T_TUPLE */
    int size, align; /* words */
    int stride;      /* T_VEC / T_LIST element stride in words */
    int data_off;    /* T_LIST: word offset of data[] */
    u32 spv_id;      /* codegen cache (per module, reset by codegen) */
    u32 spv_buf_id;  /* codegen: explicitly laid out variant for the state buffer */
    u32 spv_buf_aux; /* codegen: T_LIST: the laid-out data array type */
    Type *next;      /* intern list */
};

extern Type *t_error, *t_void, *t_bool, *t_int, *t_real, *t_complex, *t_quat, *t_string, *t_color;
void types_init(void);
Type *type_vec(Type *elem, int n);
Type *type_list(Type *elem, int cap);
Type *type_tuple(Type **items, int n); /* items copied */
bool type_is_number(Type *t);           /* Z R C H */
bool type_is_scalar_real(Type *t);      /* Z R */
int type_number_rank(Type *t);          /* Z=1 R=2 C=3 H=4, else 0 */
Type *type_number_of_rank(int r);
void type_name(Type *t, StrBuf *out);   /* "R^3", "List of C", ... */
const char *type_str(Type *t);          /* static rotating buffer, for messages */

/* ---- AST ---------------------------------------------------------------- */
typedef struct Sym Sym;
typedef struct Func Func;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Section Section;

typedef enum ExprKind {
    E_INT, E_REAL, E_IMAG, E_BOOL, E_STRING,
    E_NAME,      /* variable (sym) */
    E_INTERNAL,  /* .name (resolved to sym or constant) */
    E_MEMBER,    /* viewer.prop (resolved to sym) */
    E_UNARY,     /* op: '-' or TK_NOT */
    E_BINARY,    /* op: TK_PLUS.. TK_GE, TK_AND, TK_OR, TK_MOD */
    E_POWI,      /* a ^ n with n a small integer constant (ival) */
    E_ABS,       /* |a| */
    E_PRED,      /* a escapes / a vanishes (op = TK_ESCAPES / TK_VANISHES) */
    E_CALL,      /* user/prelude function call (fn) or intrinsic (intrinsic != 0) */
    E_INDEX,     /* a[b], 1-based */
    E_VECTOR,    /* [a, b, c] */
    E_TUPLE,     /* (a, b) -- only as function results / destructuring */
    E_CONVERT,   /* implicit numeric promotion of a to type */
    E_COLOR_CONST, /* constant of any number/vector type (cval) */
    E_SOLVE,     /* critical / root / derivative (op = SOLVE_*); rewritten by the checker into a call */
    E_COND       /* if args[0] then args[1] else args[2] */
} ExprKind;

struct Expr {
    ExprKind kind;
    int line, col, end_col;
    Type *type;        /* set by the checker */
    int op;            /* operator token / predicate / intrinsic id */
    Expr *a, *b;
    Expr **args;       /* call args, vector items, tuple items */
    int nargs;
    char *name;        /* E_NAME, E_INTERNAL, E_MEMBER (prop), E_CALL */
    char *base_name;   /* E_MEMBER: viewer name */
    Sym *sym;          /* resolved variable */
    Func *fn;          /* resolved function */
    int intrinsic;     /* E_CALL: intrinsic id (>0) */
    i32 ival;          /* E_INT, E_BOOL, E_POWI exponent */
    double rval;       /* E_REAL / E_IMAG */
    float cval[4];     /* E_COLOR_CONST and folded constants */
    bool paren;        /* written in parentheses (for juxtaposition diagnostics) */
    Expr *cont_from;   /* E_SOLVE ... continuing from [VAR =] P: the point P */
    char *cont_var;    /* ... and VAR (NULL: the viewer's variable) */
};

enum { SOLVE_CRITICAL = 1, SOLVE_ROOT, SOLVE_DERIVATIVE };
/* how a pixel's `continuing` solve starts (flood.c) */
enum { FLOOD_DIRECT = 0, FLOOD_START = 1, FLOOD_CONTINUE = 2 };
#define FLOOD_GROUP_SIZE 64 /* compute workgroup size of flood shaders */
/* the passes of a flood fill (flood.c), also the `mode` push constant of its compute shader */
enum { FLOOD_PASS_RING = 0, FLOOD_PASS_COLD = 1, FLOOD_PASS_DECIDE = 2, FLOOD_PASS_MEND = 3 };
#define FLOOD_MEND_KMAX 32 /* thickest band mended (the limit grows with the picture, see flood_mend_limit) */
#define FLOOD_MEND_JUMPS 3 /* jumps looked for on each side of a pixel */
#define FLOOD_MEND_HOPS 4  /* a pixel continues from its source's source (...) when that is mended too */
#define FS_FOR_LIMIT 1000000 /* a `for` loop runs its whole range, up to this many steps (while loops: .maxIterations) */

typedef struct Block {
    Stmt **stmts;
    int n;
} Block;

typedef enum StmtKind {
    S_EXPR, S_DECL, S_ASSIGN, S_DESTRUCTURE, S_IF, S_WHILE, S_DOWHILE, S_FOR, S_FOREACH,
    S_BREAK, S_CONTINUE, S_RETURN, S_DRAW, S_DEFINE, S_APPEND,
    S_ON,    /* tools: on EVENT do body end (op = the TOOL_EV_*) */
    S_ERASE, /* tools: erase the tool's drawings */
    S_PRINT  /* print TEXT: a line in the Info pane (text: texts in quotes and values joined with +) */
} StmtKind;

/* the events a tool can handle with `on EVENT do ... end` */
enum { TOOL_EV_PRESS, TOOL_EV_DRAG, TOOL_EV_RELEASE, TOOL_EV_CLICK, TOOL_EV_HOVER, TOOL_EV_SHOW, TOOL_EV_HIDE, TOOL_EV_COUNT };

typedef enum DrawShape { DRAW_POINT, DRAW_CIRCLE, DRAW_DISK, DRAW_LINE, DRAW_TEXT } DrawShape;

typedef struct TypeSyntax TypeSyntax; /* parsed type annotation */
struct TypeSyntax {
    char *base;       /* "Z", "R", "C", "H", "Color", "Bool", "List", ... */
    int power;        /* R^n : n, 0 if none */
    TypeSyntax *elem; /* List of <elem> */
    int cap;          /* List capacity (0 = default) */
    int line, col;
};

struct Stmt {
    StmtKind kind;
    int line, col, end_col;
    Expr *target;       /* S_DECL/S_ASSIGN/S_APPEND: lvalue */
    Expr *value;        /* initializer / assigned value / condition (S_IF/S_WHILE/S_DOWHILE) */
    int op;             /* S_ASSIGN: TK_ASSIGN, TK_PLUSEQ, ... */
    TypeSyntax *tsyn;   /* S_DECL: annotation (NULL for :=) */
    Sym *sym;           /* declared symbol (S_DECL, S_FOR, S_FOREACH) */
    Expr *range_lo, *range_hi; /* @config slider range `in a..b` */
    char *label;               /* @config: the text after a declaration, shown instead of the name */
    char **choices; int nchoices; /* @config: `in ["a", "b"]`, a choice (the value is 1, 2, ...) */
    Block body, else_body;
    Expr *from, *to, *step;    /* S_FOR range; S_FOREACH uses `from` as the collection */
    Sym *loop_end, *loop_step; /* hidden locals for S_FOR */
    Expr **targets; int ntargets; bool declare; /* S_DESTRUCTURE */
    /* S_DRAW */
    DrawShape shape;
    Expr *p0, *p1, *radius, *color, *width;
    Expr *text;         /* draw text: "..." + values */
    bool radius_px;
    char *in_viewer; int viewer_index;
    u64 erase_tools;    /* S_ERASE "Name" (op 2): the tools with that name (bit = tool index) */
    Func *fn;           /* S_DEFINE */
    bool internal;      /* generated code: may assign .stuck / .iterations */
    bool quiet;         /* generated loop: no .maxIterations limit, leaves .stuck / .iterations alone */
};

/* ---- symbols --------------------------------------------------------- */
typedef enum SymKind {
    SYM_GLOBAL,   /* program state (lives in the state buffer) */
    SYM_LOCAL,    /* section or function local (frame slot) */
    SYM_EXEC,     /* per-execution internals: .stuck .iterations */
    SYM_CONST     /* read-only constants (.red, pi) */
} SymKind;

struct Sym {
    char *name;
    Type *type;
    SymKind kind;
    int slot;          /* word offset: state buffer (globals) or frame (locals) */
    int line, col;
    bool is_config;    /* declared in @config: shown in the config panel */
    bool is_internal;  /* .name */
    bool readonly;
    bool untyped;      /* declared without a type (x := e, untyped parameters and outputs): it can get wider (TypeHint) */
    bool unknown_type; /* an untyped output nothing was assigned to yet (its type is a placeholder) */
    u64 hint_ctx;      /* the instance of a generic function it belongs to (0: none), for its TypeHint */
    bool has_range;    /* config slider range */
    double range_lo, range_hi;
    char *label;       /* config label (NULL: the name) */
    char **choices; int nchoices; /* config choice: value k shows choices[k - 1] */
    float cval[4];     /* SYM_CONST value */
    Func *owner;       /* function owning this local, NULL for section locals */
    int viewer_index;  /* viewer property/picked point: owning viewer, else -1 */
    const char *doc;   /* short description (internals) */
    u32 spv_var;       /* codegen: variable id for the current module */
    bool used;         /* referenced by the section being compiled (codegen) */
};
typedef VEC(Sym *) SymVec;

/* ---- functions --------------------------------------------------------- */
struct Func {
    char *name;
    int line, col;
    bool prelude;
    Sym **params; int nparams;
    TypeSyntax **param_tsyn;
    Sym **outputs; int noutputs;
    TypeSyntax **output_tsyn;
    Expr *body_expr;    /* `define f(x) = expr` */
    Block body;         /* `define f(x) = (u, v) where ... end` */
    bool has_where;
    Type *ret;          /* single output type or tuple */
    SymVec locals;      /* every frame slot: params, outputs, locals */
    int frame_words;
    int check_state;    /* 0 unchecked, 1 in progress, 2 done */
    bool failed;
    bool uses_draw;
    Func *next_overload;
    bool shadowed;      /* a built-in function the script defines again (the script's one is used) */
    int src_start, src_end; /* one-line functions of the script: byte range of the expression in the source */
    u32 spv_fn;         /* codegen */
    bool spv_emitted;
    int ncalls;
    /* generic functions: a parameter without a type takes the type of the argument.  The function as written is a
       template that is never checked or run itself; each list of argument types gets its own copy, an instance. */
    bool generic;       /* the template */
    Func *tmpl;         /* an instance: its template */
    Func *instances, *next_instance; /* the template's instances */
    Type **arg_types;   /* an instance: the argument types it is for */
    u64 ctx;            /* an instance: a key for the TypeHints of its variables */
    int call_line;      /* an instance: the first call */
    bool probe;         /* an instance made to check a template nothing calls */
};
typedef VEC(Func *) FuncVec;

/* ---- sections ----------------------------------------------------------- */
typedef enum SectionKind { SEC_GLOBAL, SEC_CONFIG, SEC_VIEWER, SEC_TOOL, SEC_PREPARE } SectionKind;

struct Section {
    SectionKind kind;
    int line;
    int index;          /* viewer index / tool index within its kind */
    char *name;         /* viewer name */
    char *var;          /* viewer coordinate variable */
    char *display;      /* display name */
    char *shortcut;     /* "Ctrl + 1" */
    bool projective;
    Block body;
    SymVec locals;
    int frame_words;
    Sym *coord;         /* viewer coordinate variable */
    Sym *color;         /* viewer output */
    Sym *picked;        /* viewer: .var (picked point) */
    Sym *center, *radius; /* viewer properties */
    Sym *center_lo;     /* flat viewers: the low part of the centre, for deep zooms (centre + center_lo) */
    Sym *mobius;        /* projective viewers: view map, C^4 (a, b, c, d) */
    bool panning, hover, keep; /* tool settings */
    bool button;        /* tool: `button` in the header, an action run once (not selected) */
    Block *on[TOOL_EV_COUNT]; /* tool: the handler of each event (NULL: none) */
    bool has_events;    /* tool: written with `on ... do` blocks */
    bool has_main;      /* tool: has statements outside the blocks (besides its settings) */
    char *icon;         /* tool: Material Icons name ("timeline"), NULL = first letter */
    char *of_viewer;    /* tool: written @NAME.tool, the viewer it belongs to (NULL = every viewer) */
    int viewer_of;      /* tool: index of that viewer, -1 = every viewer */
    bool uses_time, uses_mouse; /* viewer depends on .time / .mouse */
    bool implicit;      /* the whole file was this (default) viewer */
    Func *flood;        /* viewer: the function of its `continuing` solve (rendered by flood fill), or NULL */
    Expr *flood_start;  /* ... and the point the fill starts from (`continuing from c = P`: P) */
};
typedef VEC(Section *) SectionVec;

/* ---- program ------------------------------------------------------------ */
/* the config panel, in the order of the script: titled groups (@config "Title"), notes (a line of
   text in quotes) and variables */
typedef enum ConfigItemKind { CFG_GROUP, CFG_NOTE, CFG_VAR, CFG_FORMULA } ConfigItemKind;
typedef struct ConfigItem { ConfigItemKind kind; const char *text; Sym *sym; struct Func *fn; } ConfigItem;
typedef VEC(ConfigItem) ConfigItemVec;

typedef struct Program {
    Arena arena;
    const char *source;
    const char *path;
    TokenVec tokens;
    SectionVec sections;
    Section **viewers; int nviewers;
    Section **tools; int ntools;
    Section **prepares; int nprepares; /* @prepare sections, in order */
    FuncVec funcs;       /* all user + prelude functions (overloads chained) */
    SymVec globals;      /* state variables in layout order */
    SymVec consts;
    ConfigItemVec config; /* what the config panel shows, in order */
    int state_words;     /* size of the state buffer in words */
    u32 *initial_state;  /* state after running @global/@config */
    char *load_output;   /* what @global and @config printed (malloc'ed, NULL: nothing) */
    DiagVec diags;
    int nerrors;
    /* internals with fixed meaning */
    Sym *s_max_iter, *s_epsilon, *s_escape, *s_time, *s_last_click, *s_mouse, *s_width, *s_height, *s_drag_start;
    Sym *s_stuck, *s_iterations, *s_pixel;
    Sym *s_double;          /* .doublePrecision: deep zooms in double-single precision (the app and the CLI read it) */
    char *title;
    int prelude_lines;   /* diagnostics inside the prelude are reported as internal errors */
    /* type inference (check.c): a variable declared without a type takes the widest type assigned to it; the
       checker runs again (program_load) with what an earlier run found out, as `hints` */
    struct TypeHint *hints; int nhints, cap_hints;
    bool new_hints;      /* this run found a variable that must be wider: run again */
    bool final_round;    /* (the last run: report everything) */
} Program;

/* the variable declared at (line, col) holds `type` (or wider) */
typedef struct TypeHint { int line, col; u64 ctx; Type *type; } TypeHint;

void prog_diag(Program *p, DiagSeverity sev, int line, int col, int end_col, const char *fmt, ...) FS_PRINTF(6, 7);

#endif
