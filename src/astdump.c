/* astdump.c - print checked AST in (roughly) the script syntax, for debugging generated code
 * (FS_DUMP_FUNCS=1 fractalstream --check script prints every generated function). */

static void dump_expr(Expr *e, StrBuf *o);

static const char *dump_op(int op) {
    switch (op) {
    case TK_PLUS: return "+";
    case TK_MINUS: return "-";
    case TK_STAR: return "*";
    case TK_SLASH: return "/";
    case TK_MOD: return "mod";
    case TK_EQ: return "==";
    case TK_NE: return "!=";
    case TK_LT: return "<";
    case TK_LE: return "<=";
    case TK_GT: return ">";
    case TK_GE: return ">=";
    case TK_AND: return "and";
    case TK_OR: return "or";
    default: return "?";
    }
}

static void dump_sym(Sym *s, StrBuf *o) {
    if (!s) {
        sb_puts(o, "<?>");
        return;
    }
    sb_printf(o, "%s%s", s->is_internal ? "." : "", s->name);
}

static void dump_expr(Expr *e, StrBuf *o) {
    if (!e) {
        sb_puts(o, "<null>");
        return;
    }
    switch (e->kind) {
    case E_INT: sb_printf(o, "%d", e->ival); break;
    case E_REAL: sb_printf(o, "%g", e->rval); break;
    case E_IMAG: sb_printf(o, "%gi", e->rval); break;
    case E_BOOL: sb_puts(o, e->ival ? "true" : "false"); break;
    case E_STRING: sb_printf(o, "\"%s\"", e->name); break;
    case E_NAME: case E_INTERNAL: case E_MEMBER: dump_sym(e->sym, o); break;
    case E_UNARY:
        sb_puts(o, e->op == TK_NOT ? "not " : "-");
        sb_putc(o, '(');
        dump_expr(e->a, o);
        sb_putc(o, ')');
        break;
    case E_BINARY:
        sb_putc(o, '(');
        dump_expr(e->a, o);
        sb_printf(o, " %s ", dump_op(e->op));
        dump_expr(e->b, o);
        sb_putc(o, ')');
        break;
    case E_POWI:
        sb_putc(o, '(');
        dump_expr(e->a, o);
        sb_printf(o, ")^%d", e->ival);
        break;
    case E_ABS:
        sb_putc(o, '|');
        dump_expr(e->a, o);
        sb_putc(o, '|');
        break;
    case E_PRED:
        dump_expr(e->a, o);
        sb_puts(o, e->op == TK_ESCAPES ? " escapes" : " vanishes");
        break;
    case E_CALL:
        sb_printf(o, "%s(", e->intrinsic ? (e->name ? e->name : "intrinsic") : e->fn ? e->fn->name : e->name);
        if (e->intrinsic && (!e->name || !strcmp(e->name, "(internal)"))) sb_printf(o, "#%d: ", e->intrinsic);
        for (int i = 0; i < e->nargs; i++) {
            if (i) sb_puts(o, ", ");
            dump_expr(e->args[i], o);
        }
        sb_putc(o, ')');
        break;
    case E_INDEX:
        dump_expr(e->a, o);
        sb_putc(o, '[');
        dump_expr(e->b, o);
        sb_putc(o, ']');
        break;
    case E_VECTOR: case E_TUPLE:
        sb_putc(o, e->kind == E_VECTOR ? '[' : '(');
        for (int i = 0; i < e->nargs; i++) {
            if (i) sb_puts(o, ", ");
            dump_expr(e->args[i], o);
        }
        sb_putc(o, e->kind == E_VECTOR ? ']' : ')');
        break;
    case E_CONVERT:
        sb_printf(o, "%s(", type_str(e->type));
        dump_expr(e->a, o);
        sb_putc(o, ')');
        break;
    case E_COLOR_CONST:
        if (e->type == t_complex) sb_printf(o, "(%g%+gi)", e->cval[0], e->cval[1]);
        else sb_printf(o, "const<%s>(%g, %g, %g, %g)", type_str(e->type), e->cval[0], e->cval[1], e->cval[2], e->cval[3]);
        break;
    case E_SOLVE: sb_puts(o, "<solve>"); break;
    case E_COND:
        sb_puts(o, "(if ");
        dump_expr(e->args[0], o);
        sb_puts(o, " then ");
        dump_expr(e->args[1], o);
        sb_puts(o, " else ");
        dump_expr(e->args[2], o);
        sb_putc(o, ')');
        break;
    }
}

static void dump_block(Block *b, int ind, StrBuf *o);

static void dump_stmt(Stmt *s, int ind, StrBuf *o) {
    for (int i = 0; i < ind; i++) sb_puts(o, "    ");
    switch (s->kind) {
    case S_EXPR:
        if (s->value) dump_expr(s->value, o);
        sb_putc(o, '\n');
        break;
    case S_DECL: case S_ASSIGN:
        dump_expr(s->target, o);
        sb_puts(o, s->internal ? " =(internal) " : " = ");
        if (s->value) dump_expr(s->value, o);
        else sb_puts(o, "0");
        sb_putc(o, '\n');
        break;
    case S_DESTRUCTURE:
        sb_putc(o, '(');
        for (int i = 0; i < s->ntargets; i++) {
            if (i) sb_puts(o, ", ");
            dump_expr(s->targets[i], o);
        }
        sb_puts(o, ") = ");
        dump_expr(s->value, o);
        sb_putc(o, '\n');
        break;
    case S_IF:
        sb_puts(o, "if ");
        dump_expr(s->value, o);
        sb_puts(o, " then\n");
        dump_block(&s->body, ind + 1, o);
        if (s->else_body.n) {
            for (int i = 0; i < ind; i++) sb_puts(o, "    ");
            sb_puts(o, "else\n");
            dump_block(&s->else_body, ind + 1, o);
        }
        for (int i = 0; i < ind; i++) sb_puts(o, "    ");
        sb_puts(o, "end\n");
        break;
    case S_WHILE:
        sb_printf(o, "while%s ", s->quiet ? "(quiet)" : "");
        dump_expr(s->value, o);
        sb_puts(o, " do\n");
        dump_block(&s->body, ind + 1, o);
        for (int i = 0; i < ind; i++) sb_puts(o, "    ");
        sb_puts(o, "end\n");
        break;
    case S_DOWHILE:
        sb_puts(o, "do\n");
        dump_block(&s->body, ind + 1, o);
        for (int i = 0; i < ind; i++) sb_puts(o, "    ");
        sb_puts(o, "while ");
        dump_expr(s->value, o);
        sb_puts(o, " end\n");
        break;
    case S_FOR: case S_FOREACH:
        sb_puts(o, "for ");
        dump_sym(s->sym, o);
        sb_puts(o, " in ");
        dump_expr(s->from, o);
        if (s->kind == S_FOR) {
            sb_puts(o, "..");
            dump_expr(s->to, o);
            if (s->step) {
                sb_puts(o, " by ");
                dump_expr(s->step, o);
            }
        }
        sb_puts(o, " do\n");
        dump_block(&s->body, ind + 1, o);
        for (int i = 0; i < ind; i++) sb_puts(o, "    ");
        sb_puts(o, "end\n");
        break;
    case S_BREAK: sb_puts(o, "break\n"); break;
    case S_CONTINUE: sb_puts(o, "continue\n"); break;
    case S_RETURN: sb_puts(o, "return\n"); break;
    case S_DRAW: sb_puts(o, "draw ...\n"); break;
    case S_PRINT: sb_puts(o, "print ...\n"); break;
    case S_ERASE:
        if (s->op == 2) sb_printf(o, "erase \"%s\"\n", s->label);
        else sb_puts(o, s->op ? "erase all\n" : "erase\n");
        break;
    case S_ON: sb_puts(o, "on ... do\n"); dump_block(&s->body, ind + 1, o); break;
    case S_DEFINE: sb_printf(o, "define %s ...\n", s->fn ? s->fn->name : "?"); break;
    case S_APPEND: sb_puts(o, "append ...\n"); break;
    }
}

static void dump_block(Block *b, int ind, StrBuf *o) {
    for (int i = 0; i < b->n; i++) dump_stmt(b->stmts[i], ind, o);
}

void ast_dump_func(Func *f, StrBuf *o) {
    sb_printf(o, "define %s(", f->name);
    for (int i = 0; i < f->nparams; i++) sb_printf(o, "%s%s : %s", i ? ", " : "", f->params[i]->name, type_str(f->params[i]->type));
    sb_puts(o, ") = ");
    if (f->body_expr) {
        dump_expr(f->body_expr, o);
        sb_putc(o, '\n');
        return;
    }
    sb_putc(o, '(');
    for (int i = 0; i < f->noutputs; i++) sb_printf(o, "%s%s", i ? ", " : "", f->outputs[i]->name);
    sb_printf(o, ") where   # frame %d words\n", f->frame_words);
    dump_block(&f->body, 1, o);
    sb_puts(o, "end\n");
}
