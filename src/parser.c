/*
 * parser.c - Recursive-descent parser with operator precedence.
 *
 * Gramatica resumida:
 *   program    := { directive | class | function | statement }
 *   class      := mods 'class' ID ['extends' ID] '{' { member } '}'
 *   statement  := block | if | while | do | for | return | break | continue
 *                 | try | throw | declaration | expression ';'
 *   for        := 'for' '(' init ('to' expr ';' step | ';' cond ';' step) ')' statement
 *                 init can be:  int i=0 | int i | int _ | _ | expr | empty
 *
 * Syntax errors use longjmp to return to the last recovery point
 * (statement, class member or top-level item) and keep parsing.
 */
#include "parser.h"
#include <setjmp.h>

/* ---------------- estado ---------------- */
static Token *T;
static int    P;                 /* posicao corrente */
static jmp_buf *g_rec;           /* ponto de recuperacao */
static Vec    g_classes;         /* known class names (char*) */
static int    g_func_depth;      /* >0 inside a function/method body */

static Token *cur(void) { return &T[P]; }
static Token *peekn(int n) {
    int i = P;
    while (n-- > 0 && T[i].kind != TK_EOF) i++;
    return &T[i];
}
static TokKind K(void) { return T[P].kind; }
static Loc LOC(void) { return T[P].loc; }
static void adv(void) { if (T[P].kind != TK_EOF) P++; }
static bool accept(TokKind k) { if (K() == k) { adv(); return true; } return false; }

static void fail(Loc loc, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void fail(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = NULL;
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    msg = xcalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(msg, (size_t)n + 1, fmt, ap);
    va_end(ap);
    diag_error(loc, "%s", msg);
    longjmp(*g_rec, 1);
}

static void expect(TokKind k, const char *ctx) {
    if (K() == k) { adv(); return; }
    /* for ';' the error points to the end of the previous token (more useful) */
    Loc l = LOC();
    if (k == P_SEMI && P > 0) {
        Token *prev = &T[P - 1];
        l = prev->loc;
        l.col += prev->kind == TK_STRING ? prev->len + 2 : (prev->text ? (int)strlen(prev->text) : 1);
    }
    fail(l, "expected %s %s, found %s", token_kind_name(k), ctx, token_kind_name(K()));
}

static bool is_class_name(const char *s) {
    for (int i = 0; i < g_classes.n; i++) if (strcmp(g_classes.v[i], s) == 0) return true;
    return false;
}

void parser_prescan(TokenList *tl) {
    for (int i = 0; i + 1 < tl->n; i++)
        if (((tl->t[i].kind == KW_CLASS || tl->t[i].kind == KW_INTERFACE) && tl->t[i + 1].kind == TK_IDENT) ||
            (tl->t[i].kind == TK_IDENT && !strcmp(tl->t[i].text, "enum") && tl->t[i + 1].kind == TK_IDENT && tl->t[i + 2].kind == P_LBRACE))
            vec_push(&g_classes, tl->t[i + 1].text);
}

/* ---------------- tipos ---------------- */
static bool is_prim_type_kw(TokKind k) {
    return k == KW_INT || k == KW_LONG || k == KW_FLOAT || k == KW_DOUBLE || k == KW_BOOL ||
           k == KW_CHAR || k == KW_STRING || k == KW_VOID || k == KW_PTR;
}

/* Is there a type starting at position 'i'? (used to tell declarations apart) */
static bool type_starts_at(int i) {
    TokKind k = T[i].kind;
    if (is_prim_type_kw(k)) return true;
    if (k != TK_IDENT) return false;
    TokKind n1 = T[i + 1].kind;
    if (n1 == P_LT && (!strcmp(T[i].text, "List") || !strcmp(T[i].text, "Map"))) return true;   /* List<int> x */
    if (n1 == TK_IDENT) return true;                                  /* Foo x */
    if (n1 == P_LBRACK && T[i + 2].kind == P_RBRACK) return true;     /* Foo[] x */
    if (n1 == P_STAR && is_class_name(T[i].text) &&
        (T[i + 2].kind == TK_IDENT || T[i + 2].kind == P_STAR)) return true;  /* Foo* x */
    return false;
}

static Type *parse_type(void);
static Type *parse_type_base(void) {
    Loc loc = LOC();
    Type *t = NULL;
    switch (K()) {
    case KW_INT: case KW_LONG: t = TY_int_t; break;
    case KW_FLOAT: case KW_DOUBLE: t = TY_float_t; break;
    case KW_BOOL: t = TY_bool_t; break;
    case KW_CHAR: t = TY_char_t; break;
    case KW_STRING: t = TY_string_t; break;
    case KW_VOID: t = TY_void_t; break;
    case KW_PTR: t = TY_ptrany_t; break;
    case TK_IDENT: t = type_class_named(cur()->text, loc); break;
    default: fail(loc, "expected a type, found %s", token_kind_name(K()));
    }
    adv();
    if (t->kind == TY_CLASS && K() == P_LT && (!strcmp(t->name, "List") || !strcmp(t->name, "Map"))) {
        bool is_map = !strcmp(t->name, "Map");
        adv();
        Type *a = parse_type();
        if (is_map) {
            if (a->kind != TY_STRING) fail(loc, "Map requires a String key: Map<String, %s>", "T");
            expect(P_COMMA, "between the key and the value of Map<String, T>");
            t->elem = parse_type();
        } else t->elem = a;
        if (t->elem->kind == TY_VOID) fail(loc, "the element type cannot be void");
        if (K() == P_SHR) T[P].kind = P_GT;        /* '>>' closing two generics: consumes only one '>' */
        else expect(P_GT, "to close the type arguments");
    }
    return t;
}

static Type *parse_type(void) {
    Type *t = parse_type_base();
    for (;;) {
        if (K() == P_STAR) { adv(); t = type_ptr_to(t); }
        else if (K() == P_LBRACK && peekn(1)->kind == P_RBRACK) { adv(); adv(); t = type_array_of(t); }
        else break;
    }
    return t;
}

/* ---------------- expressions ---------------- */
static Node *parse_expr(void);
static Node *parse_unary(void);
static Node *parse_array_literal(Type *declared);

static Node *mk(NodeKind k, Loc l) { return node_new(k, l); }

static void parse_args(Node *call) {
    expect(P_LPAREN, "to open the arguments");
    if (K() != P_RPAREN) {
        do vec_push(&call->list, parse_expr()); while (accept(P_COMMA));
    }
    expect(P_RPAREN, "to close the arguments");
}

static Node *parse_new(void) {
    Loc loc = LOC();
    adv();   /* new */
    Type *base = parse_type_base();
    if (K() == P_LBRACK && peekn(1)->kind == P_RBRACK) {   /* new T[]{a, b, c} */
        adv(); adv();
        if (K() != P_LBRACE) fail(LOC(), "expected '{' with the elements after 'new %s[]'", type_str(base));
        return parse_array_literal(type_array_of(base));
    }
    if (K() == P_LBRACK) {                         /* new T[n] */
        adv();
        Node *n = mk(N_NEWARRAY, loc);
        n->a = parse_expr();
        expect(P_RBRACK, "after the array size");
        n->type = type_array_of(base);
        if (K() == P_LBRACK) {                          /* segunda dimensao */
            adv();
            n->type = type_array_of(n->type);
            if (K() != P_RBRACK) n->b = parse_expr();    /* new T[n][m]; new T[n][] = array de arrays nulos */
            expect(P_RBRACK, "after the second dimension");
        }
        return n;
    }
    if (base->kind != TY_CLASS) fail(loc, "'new' with type '%s' requires an array size: new %s[n]", type_str(base), type_str(base));
    Node *n = mk(N_NEW, loc);
    n->name = base->name;
    n->type = base;
    parse_args(n);
    return n;
}

static Node *parse_array_literal(Type *declared) {
    Node *n = mk(N_ARRLIT, LOC());
    n->type = declared;
    expect(P_LBRACE, "to open the array literal");
    if (K() != P_RBRACE) {
        do {
            if (K() == P_RBRACE) break;       /* virgula final permitida */
            vec_push(&n->list, parse_expr());
        } while (accept(P_COMMA));
    }
    expect(P_RBRACE, "to close the array literal");
    return n;
}

static Node *parse_primary(void) {
    Loc loc = LOC();
    Token *t = cur();
    Node *n;
    switch (t->kind) {
    case TK_INT:    n = mk(N_INT, loc);   n->ival = t->ival; adv(); return n;
    case TK_FLOAT:  n = mk(N_FLOAT, loc); n->fval = t->fval; adv(); return n;
    case TK_CHAR:   n = mk(N_CHAR, loc);  n->ival = t->ival; adv(); return n;
    case TK_STRING: n = mk(N_STR, loc);   n->name = t->text; n->ival = t->len; adv(); return n;
    case KW_TRUE:   n = mk(N_BOOL, loc);  n->ival = 1; adv(); return n;
    case KW_FALSE:  n = mk(N_BOOL, loc);  n->ival = 0; adv(); return n;
    case KW_NULL:   n = mk(N_NULL, loc);  adv(); return n;
    case KW_THIS:   n = mk(N_THIS, loc);  adv(); return n;
    case KW_SUPER:  n = mk(N_SUPER, loc); adv(); return n;
    case TK_IDENT:  n = mk(N_IDENT, loc); n->name = t->text; adv(); return n;
    case KW_NEW:    return parse_new();
    case P_LBRACE:  return parse_array_literal(NULL);
    case KW_SIZEOF:
        adv();
        n = mk(N_SIZEOF, loc);
        expect(P_LPAREN, "after 'sizeof'");
        n->type = parse_type();
        expect(P_RPAREN, "after the type in 'sizeof'");
        return n;
    case P_LPAREN:
        adv();
        n = parse_expr();
        expect(P_RPAREN, "to close the parenthesis");
        return n;
    default:
        fail(loc, "expected an expression, found %s", token_kind_name(t->kind));
    }
}

static Node *parse_postfix(void) {
    Node *e = parse_primary();
    for (;;) {
        Loc loc = LOC();
        if (K() == P_LPAREN) {
            Node *c = mk(N_CALL, loc);
            c->a = e;
            parse_args(c);
            e = c;
        } else if (K() == P_LBRACK) {
            adv();
            Node *i = mk(N_INDEX, loc);
            i->a = e;
            i->b = parse_expr();
            expect(P_RBRACK, "after the index");
            e = i;
        } else if (K() == P_DOT) {
            adv();
            if (K() != TK_IDENT) fail(LOC(), "expected a member name after '.', found %s", token_kind_name(K()));
            Node *m = mk(N_MEMBER, loc);
            m->a = e;
            m->name = cur()->text;
            adv();
            e = m;
        } else if (K() == P_PLUSPLUS || K() == P_MINUSMINUS) {
            Node *i = mk(N_INCDEC, loc);
            i->op = K(); i->a = e; i->flags |= NF_POST;
            adv();
            e = i;
        } else break;
    }
    return e;
}

/* '(' is already the current token; decides whether it is a cast */
static bool looks_like_cast(void) {
    Token *t1 = peekn(1);
    if (is_prim_type_kw(t1->kind) && t1->kind != KW_VOID) return true;
    if (t1->kind == TK_IDENT && is_class_name(t1->text)) {
        int i = P + 2;
        while (T[i].kind == P_STAR || (T[i].kind == P_LBRACK && T[i + 1].kind == P_RBRACK)) i += (T[i].kind == P_STAR) ? 1 : 2;
        if (T[i].kind != P_RPAREN) return false;
        switch (T[i + 1].kind) {
        case TK_IDENT: case TK_INT: case TK_FLOAT: case TK_STRING: case TK_CHAR: case P_LPAREN:
        case P_BANG: case P_TILDE: case KW_NEW: case KW_THIS: case KW_NULL: return true;
        default: return false;
        }
    }
    return false;
}

static Node *parse_unary(void) {
    Loc loc = LOC();
    switch (K()) {
    case P_MINUS: case P_BANG: case P_TILDE: case P_STAR: case P_AMP: {
        Node *n = mk(N_UNARY, loc);
        n->op = K();
        adv();
        n->a = parse_unary();
        return n;
    }
    case P_PLUS: adv(); return parse_unary();
    case P_PLUSPLUS: case P_MINUSMINUS: {
        Node *n = mk(N_INCDEC, loc);
        n->op = K();
        adv();
        n->a = parse_unary();
        return n;
    }
    case P_LPAREN:
        if (looks_like_cast()) {
            adv();
            Node *n = mk(N_CAST, loc);
            n->type = parse_type();
            expect(P_RPAREN, "after the cast type");
            n->a = parse_unary();
            return n;
        }
        /* fallthrough */
    default:
        return parse_postfix();
    }
}

static int binary_prec(TokKind k) {
    switch (k) {
    case P_OROR: return 1;
    case P_ANDAND: return 2;
    case P_PIPE: return 3;
    case P_CARET: return 4;
    case P_AMP: return 5;
    case P_EQ: case P_NE: return 6;
    case P_LT: case P_LE: case P_GT: case P_GE: case KW_INSTANCEOF: return 7;
    case P_SHL: case P_SHR: return 8;
    case P_PLUS: case P_MINUS: return 9;
    case P_STAR: case P_SLASH: case P_PERCENT: return 10;
    default: return 0;
    }
}

static Node *parse_binary(int minprec) {
    Node *lhs = parse_unary();
    for (;;) {
        int prec = binary_prec(K());
        if (prec == 0 || prec < minprec) return lhs;
        if (K() == KW_INSTANCEOF) {
            Node *io = mk(N_INSTANCEOF, LOC());
            adv();
            io->a = lhs;
            io->type = parse_type();
            lhs = io;
            continue;
        }
        Node *n = mk(N_BINARY, LOC());
        n->op = K();
        adv();
        n->a = lhs;
        n->b = parse_binary(prec + 1);
        lhs = n;
    }
}

static Node *parse_ternary(void) {
    Node *c = parse_binary(1);
    if (K() == P_QUESTION) {
        Node *n = mk(N_COND, LOC());
        adv();
        n->a = c;
        n->b = parse_expr();
        expect(P_COLON, "in the '?:' operator");
        n->c = parse_ternary();
        return n;
    }
    return c;
}

static bool is_assign_op(TokKind k) {
    switch (k) {
    case P_ASSIGN: case P_PLUSEQ: case P_MINUSEQ: case P_STAREQ: case P_SLASHEQ: case P_PERCENTEQ:
    case P_AMPEQ: case P_PIPEEQ: case P_CARETEQ: case P_SHLEQ: case P_SHREQ: return true;
    default: return false;
    }
}

static Node *parse_expr(void) {
    Node *lhs = parse_ternary();
    if (is_assign_op(K())) {
        Node *n = mk(N_ASSIGN, LOC());
        n->op = K();
        adv();
        n->a = lhs;
        n->b = parse_expr();
        return n;
    }
    return lhs;
}

/* ---------------- statements ---------------- */
static Node *parse_stmt(void);
static Node *parse_class(int access);

static Node *parse_block(void) {
    Node *b = mk(N_BLOCK, LOC());
    expect(P_LBRACE, "to open the block");
    while (K() != P_RBRACE && K() != TK_EOF) {
        /* per-statement recovery */
        jmp_buf jb;
        jmp_buf *old = g_rec;
        g_rec = &jb;
        volatile int start = P;
        if (setjmp(jb) == 0) {
            vec_push(&b->list, parse_stmt());
        } else {
            /* synchronize: skip to ';' or before '}' */
            if (P == start) adv();
            while (K() != TK_EOF && K() != P_RBRACE) {
                if (K() == P_SEMI) { adv(); break; }
                if (K() == P_LBRACE) break;
                adv();
            }
        }
        g_rec = old;
    }
    expect(P_RBRACE, "to close the block");
    return b;
}

/* declaration: <type?> name [= expr] {, name [= expr]} ; */
static Node *parse_var_decls(Type *type, int flags, bool need_semi) {
    Node *group = mk(N_BLOCK, LOC());
    group->flags |= NF_NOSCOPE;
    do {
        Loc loc = LOC();
        if (K() != TK_IDENT) fail(loc, "expected a variable name, found %s", token_kind_name(K()));
        Node *v = mk(N_VAR, loc);
        v->name = cur()->text;
        v->type = type;
        v->flags = flags;
        if (!type) v->flags |= NF_INFERRED;
        adv();
        if (accept(P_ASSIGN)) v->a = parse_expr();
        else if (!type) fail(loc, "constant '%s' needs an initial value", v->name);
        vec_push(&group->list, v);
    } while (accept(P_COMMA));
    if (need_semi) expect(P_SEMI, "at the end of the declaration");
    if (group->list.n == 1) return group->list.v[0];
    return group;
}

static Node *parse_for(void) {
    Node *n = mk(N_FOR, LOC());
    adv();
    expect(P_LPAREN, "after 'for'");
    Node *init = NULL;
    if (K() == TK_IDENT && strcmp(cur()->text, "_") == 0 && peekn(1)->kind == P_SEMI) {
        adv();
        n->flags |= NF_FOR_PREDECL;
    } else if (K() == P_SEMI) {
        /* no init */
    } else if (type_starts_at(P)) {
        Type *ty = parse_type();
        if (K() == TK_IDENT && peekn(1)->kind == P_COLON) {           /* for (T x : colecao) */
            static int fe_counter;
            int id = fe_counter++;
            Loc fl = n->loc;
            char *vname = cur()->text;
            adv(); adv();
            Node *coll = parse_expr();
            expect(P_RPAREN, "to close the 'for-each'");
            Node *body = parse_stmt();
            char *tmp = xsprintf("__fe%d", id), *idx = xsprintf("__fi%d", id);
            Node *outer = mk(N_BLOCK, fl);
            Node *vt = mk(N_VAR, fl); vt->name = tmp; vt->a = coll; vt->flags |= NF_INFERRED;
            Node *vi = mk(N_VAR, fl); vi->name = idx; vi->type = TY_int_t; vi->a = mk(N_INT, fl);
            Node *tid = mk(N_IDENT, fl); tid->name = tmp;
            Node *len = mk(N_MEMBER, fl); len->a = tid; len->name = "length";
            Node *iid = mk(N_IDENT, fl); iid->name = idx;
            Node *cond = mk(N_BINARY, fl); cond->op = P_LT; cond->a = iid; cond->b = len;
            Node *iid2 = mk(N_IDENT, fl); iid2->name = idx;
            Node *step = mk(N_INCDEC, fl); step->op = P_PLUSPLUS; step->a = iid2; step->flags |= NF_POST;
            Node *tid2 = mk(N_IDENT, fl); tid2->name = tmp;
            Node *iid3 = mk(N_IDENT, fl); iid3->name = idx;
            Node *elem = mk(N_INDEX, fl); elem->a = tid2; elem->b = iid3;
            Node *vx = mk(N_VAR, fl); vx->name = vname; vx->type = ty; vx->a = elem;
            Node *inner = mk(N_BLOCK, fl); vec_push(&inner->list, vx); vec_push(&inner->list, body);
            n->a = vi; n->b = cond; n->c = step; n->d = inner;
            vec_push(&outer->list, vt); vec_push(&outer->list, n);
            return outer;
        }
        if (K() == TK_IDENT && strcmp(cur()->text, "_") == 0) {
            adv();
            n->flags |= NF_FOR_IMPLICIT;
            n->type = ty;
        } else {
            init = parse_var_decls(ty, 0, false);
        }
    } else {
        Node *es = mk(N_EXPRSTMT, LOC());
        es->a = parse_expr();
        init = es;
    }
    n->a = init;

    if (K() == TK_IDENT && strcmp(cur()->text, "to") == 0) {      /* for(int i=0 to N; passo) */
        Loc tl = LOC();
        adv();
        Node *limit = parse_expr();
        const char *vn = NULL;
        if (init && init->kind == N_VAR) vn = init->name;
        else if (init && init->kind == N_EXPRSTMT && init->a->kind == N_ASSIGN && init->a->a->kind == N_IDENT) vn = init->a->a->name;
        if (!vn) fail(tl, "'to' requires a control variable, e.g.: for(int i = 0 to 10; i++)");
        Node *id = mk(N_IDENT, tl);
        id->name = (char *)vn;
        Node *cond = mk(N_BINARY, tl);
        cond->op = P_LE; cond->a = id; cond->b = limit;
        n->b = cond;
        if (accept(P_SEMI) && K() != P_RPAREN) n->c = parse_expr();
        if (!n->c) {                         /* passo omitido: i++ */
            Node *id2 = mk(N_IDENT, tl);
            id2->name = (char *)vn;
            Node *inc = mk(N_INCDEC, tl);
            inc->op = P_PLUSPLUS; inc->a = id2; inc->flags |= NF_POST;
            n->c = inc;
        }
    } else {
        expect(P_SEMI, "after the 'for' initializer");
        if (K() != P_SEMI) n->b = parse_expr();
        expect(P_SEMI, "after the 'for' condition");
        if (K() != P_RPAREN) n->c = parse_expr();
    }
    expect(P_RPAREN, "to close the 'for'");
    g_func_depth++;
    n->d = parse_stmt();
    g_func_depth--;
    return n;
}

static Node *parse_try(void) {
    Node *n = mk(N_TRY, LOC());
    adv();
    n->a = parse_block();
    while (K() == KW_CATCH) {
        Node *c = mk(N_CATCH, LOC());
        adv();
        expect(P_LPAREN, "after 'catch'");
        c->type = parse_type();
        if (K() != TK_IDENT) fail(LOC(), "expected an exception variable name, found %s", token_kind_name(K()));
        c->name = cur()->text;
        adv();
        expect(P_RPAREN, "after the 'catch' variable");
        c->a = parse_block();
        vec_push(&n->list, c);
    }
    if (accept(KW_FINALLY)) n->b = parse_block();
    if (n->list.n == 0 && !n->b) fail(n->loc, "'try' needs at least one 'catch' or a 'finally'");
    return n;
}

static bool g_static, g_abstract, g_in_interface;

static int parse_access_mods(void) {
    int acc = ACC_PACKAGE;
    bool seen = false;
    g_static = g_abstract = false;
    while (K() == KW_PUBLIC || K() == KW_PRIVATE || K() == KW_PROTECTED || K() == KW_PACKAGE || K() == KW_STATIC || K() == KW_ABSTRACT) {
        if (K() == KW_STATIC) { g_static = true; adv(); continue; }
        if (K() == KW_ABSTRACT) { g_abstract = true; adv(); continue; }
        if (seen) diag_error(LOC(), "more than one access modifier");
        acc = K() == KW_PUBLIC ? ACC_PUBLIC : K() == KW_PRIVATE ? ACC_PRIVATE : K() == KW_PROTECTED ? ACC_PROTECTED : ACC_PACKAGE;
        seen = true;
        adv();
    }
    return acc;
}

static Node *parse_stmt(void) {
    Loc loc = LOC();
    switch (K()) {
    case P_LBRACE: return parse_block();
    case P_SEMI: adv(); return mk(N_EMPTY, loc);
    case KW_IF: {
        Node *n = mk(N_IF, loc);
        adv();
        expect(P_LPAREN, "after 'if'");
        n->a = parse_expr();
        expect(P_RPAREN, "after the 'if' condition");
        n->b = parse_stmt();
        if (accept(KW_ELSE)) n->c = parse_stmt();
        return n;
    }
    case KW_WHILE: {
        Node *n = mk(N_WHILE, loc);
        adv();
        expect(P_LPAREN, "after 'while'");
        n->a = parse_expr();
        expect(P_RPAREN, "after the 'while' condition");
        n->b = parse_stmt();
        return n;
    }
    case KW_DO: {
        Node *n = mk(N_DO, loc);
        adv();
        n->a = parse_stmt();
        if (!accept(KW_WHILE)) fail(LOC(), "expected 'while' after the 'do' body");
        expect(P_LPAREN, "after 'while'");
        n->b = parse_expr();
        expect(P_RPAREN, "after the 'do-while' condition");
        expect(P_SEMI, "at the end of the 'do-while'");
        return n;
    }
    case KW_FOR: return parse_for();
    case KW_RETURN: {
        Node *n = mk(N_RETURN, loc);
        adv();
        if (K() != P_SEMI) n->a = parse_expr();
        expect(P_SEMI, "after 'return'");
        return n;
    }
    case KW_BREAK: adv(); expect(P_SEMI, "after 'break'"); return mk(N_BREAK, loc);
    case KW_CONTINUE: adv(); expect(P_SEMI, "after 'continue'"); return mk(N_CONTINUE, loc);
    case KW_TRY: return parse_try();
    case KW_SWITCH: {
        Node *n = mk(N_SWITCH, loc);
        adv();
        expect(P_LPAREN, "after 'switch'");
        n->a = parse_expr();
        expect(P_RPAREN, "after the 'switch' expression");
        expect(P_LBRACE, "to open the 'switch'");
        while (K() != P_RBRACE && K() != TK_EOF) {
            Node *c = mk(N_CASE, LOC());
            if (accept(KW_CASE)) c->a = parse_ternary();
            else if (!accept(KW_DEFAULT)) fail(LOC(), "expected 'case' or 'default', found %s", token_kind_name(K()));
            expect(P_COLON, "after the 'case' label");
            while (K() != KW_CASE && K() != KW_DEFAULT && K() != P_RBRACE && K() != TK_EOF) vec_push(&c->list, parse_stmt());
            vec_push(&n->list, c);
        }
        expect(P_RBRACE, "to close the 'switch'");
        return n;
    }
    case KW_THROW: {
        Node *n = mk(N_THROW, loc);
        adv();
        n->a = parse_expr();
        expect(P_SEMI, "after 'throw'");
        return n;
    }
    case KW_CONST: {
        adv();
        Type *ty = NULL;
        if (type_starts_at(P)) ty = parse_type();
        return parse_var_decls(ty, NF_CONST, true);
    }
    case KW_PUBLIC: case KW_PRIVATE: case KW_PROTECTED: case KW_PACKAGE: case KW_CLASS: case KW_STATIC: case KW_ABSTRACT: case KW_INTERFACE: {
        int acc = parse_access_mods();
        if (K() != KW_CLASS && K() != KW_INTERFACE) fail(LOC(), "access modifier not allowed here (only on classes and members)");
        Node *s = mk(N_CLASSSTMT, loc);
        s->a = parse_class(acc);
        return s;
    }
    case TK_DIRECTIVE:
        fail(loc, "the directive '#%s' can only appear at top level, outside functions", cur()->text);
    default:
        break;
    }
    if (K() == TK_IDENT && strcmp(cur()->text, "var") == 0 && peekn(1)->kind == TK_IDENT) {   /* var x = expr; */
        adv();
        return parse_var_decls(NULL, 0, true);
    }
    if (type_starts_at(P)) {
        Type *ty = parse_type();
        return parse_var_decls(ty, 0, true);
    }
    Node *es = mk(N_EXPRSTMT, loc);
    es->a = parse_expr();
    expect(P_SEMI, "at the end of the statement");
    return es;
}

/* ---------------- classes and functions ---------------- */
static void parse_params(Node *fn) {
    expect(P_LPAREN, "to open the parameters");
    if (K() != P_RPAREN) {
        do {
            Node *p = mk(N_PARAM, LOC());
            p->type = parse_type();
            if (K() != TK_IDENT) fail(LOC(), "expected a parameter name, found %s", token_kind_name(K()));
            p->name = cur()->text;
            adv();
            vec_push(&fn->list, p);
        } while (accept(P_COMMA));
    }
    expect(P_RPAREN, "to close the parameters");
}

static Node *parse_func_rest(Type *ret, Loc loc, int access) {
    Node *f = mk(N_FUNC, loc);
    bool m_static = g_static, m_abstract = g_abstract, in_if = g_in_interface;
    f->type = ret;
    f->access = access;
    if (m_static) f->flags |= NF_STATIC;
    f->name = cur()->text;
    f->loc = LOC();
    adv();
    parse_params(f);
    if (K() == P_SEMI) {                               /* method without a body: abstract / interface */
        if (!m_abstract && !in_if) fail(LOC(), "method '%s' has no body; use 'abstract' (or declare it in an interface)", f->name);
        adv();
        f->flags |= NF_ABSTRACT;
        return f;
    }
    if (m_abstract) fail(LOC(), "abstract method '%s' cannot have a body", f->name);
    g_func_depth++;
    f->a = parse_block();
    g_func_depth--;
    return f;
}

static Node *parse_ident_list(const char *what) {
    Node *lst = mk(N_SEQ, LOC());
    do {
        if (K() != TK_IDENT) fail(LOC(), "expected %s, found %s", what, token_kind_name(K()));
        Node *id = mk(N_IDENT, LOC());
        id->name = cur()->text;
        vec_push(&lst->list, id);
        adv();
    } while (accept(P_COMMA));
    return lst;
}

static Node *parse_class(int access) {
    Node *c = mk(N_CLASS, LOC());
    bool is_abs = g_abstract, is_if = K() == KW_INTERFACE;
    g_abstract = g_static = false;
    c->access = access;
    if (is_abs) c->flags |= NF_ABSTRACT;
    if (is_if) c->flags |= NF_INTERFACE;
    adv();   /* class / interface */
    if (K() != TK_IDENT) fail(LOC(), "expected a class name, found %s", token_kind_name(K()));
    c->name = cur()->text;
    c->loc = LOC();
    adv();
    if (accept(KW_EXTENDS)) {
        if (is_if) c->a = parse_ident_list("interface name");
        else {
            if (K() != TK_IDENT) fail(LOC(), "expected a superclass name, found %s", token_kind_name(K()));
            c->d = mk(N_IDENT, LOC());
            c->d->name = cur()->text;
            adv();
        }
    }
    if (!is_if && K() == TK_IDENT && !strcmp(cur()->text, "implements")) {
        adv();
        c->a = parse_ident_list("interface name");
    }
    bool saved_if = g_in_interface;
    g_in_interface = is_if;
    expect(P_LBRACE, "to open the class body");
    while (K() != P_RBRACE && K() != TK_EOF) {
        jmp_buf jb;
        jmp_buf *old = g_rec;
        g_rec = &jb;
        volatile int start = P;
        if (setjmp(jb) == 0) {
            Loc mloc = LOC();
            int acc = parse_access_mods();
            bool m_static = g_static;
            if (K() == KW_CLASS || K() == KW_INTERFACE) fail(LOC(), "nested classes are not supported");
            if (K() == TK_IDENT && peekn(1)->kind == P_LPAREN) {
                /* no return type: constructor (if the name = class) or void method */
                Node *f = parse_func_rest(NULL, mloc, acc);
                if (strcmp(f->name, c->name) == 0) f->flags |= NF_CTOR;
                vec_push(&c->list, f);
            } else {
                Type *ty = parse_type();
                if (K() == TK_IDENT && peekn(1)->kind == P_LPAREN) {
                    vec_push(&c->list, parse_func_rest(ty, mloc, acc));
                } else {
                    do {
                        Node *fld = mk(N_FIELD, LOC());
                        fld->type = ty;
                        fld->access = acc;
                        if (m_static) fld->flags |= NF_STATIC;
                        if (K() != TK_IDENT) fail(LOC(), "expected a field name, found %s", token_kind_name(K()));
                        fld->name = cur()->text;
                        adv();
                        if (accept(P_ASSIGN)) fld->a = parse_expr();
                        vec_push(&c->list, fld);
                    } while (accept(P_COMMA));
                    expect(P_SEMI, "at the end of the field declaration");
                }
            }
        } else {
            if (P == start) adv();
            int depth = 0;
            while (K() != TK_EOF) {
                if (K() == P_LBRACE) depth++;
                else if (K() == P_RBRACE) { if (depth == 0) break; depth--; if (depth == 0) { adv(); break; } }
                else if (K() == P_SEMI && depth == 0) { adv(); break; }
                adv();
            }
        }
        g_rec = old;
    }
    expect(P_RBRACE, "to close the class");
    g_in_interface = saved_if;
    return c;
}

static Node *parse_enum(void) {
    Node *e = mk(N_ENUM, LOC());
    adv();                       /* enum */
    e->name = cur()->text;
    e->loc = LOC();
    adv();
    expect(P_LBRACE, "to open the enum");
    int64_t next = 0;
    while (K() != P_RBRACE && K() != TK_EOF) {
        if (K() != TK_IDENT) fail(LOC(), "expected an enum constant name, found %s", token_kind_name(K()));
        Node *c = mk(N_IDENT, LOC());
        c->name = cur()->text;
        adv();
        if (accept(P_ASSIGN)) {
            bool neg = accept(P_MINUS);
            if (K() != TK_INT) fail(LOC(), "an enum constant value must be an integer literal");
            next = neg ? -cur()->ival : cur()->ival;
            adv();
        }
        c->ival = next++;
        vec_push(&e->list, c);
        if (!accept(P_COMMA)) break;
    }
    expect(P_RBRACE, "to close the enum");
    accept(P_SEMI);
    return e;
}

static Node *parse_directive(void) {
    Node *d = mk(N_DIRECTIVE, LOC());
    d->name = cur()->text;
    adv();
    if (strcmp(d->name, "definePackage") == 0 || strcmp(d->name, "definePackege") == 0) {
        accept(P_COLON);
        do {
            if (K() != TK_IDENT) fail(LOC(), "expected a class name in the directive list, found %s", token_kind_name(K()));
            Node *id = mk(N_IDENT, LOC());
            id->name = cur()->text;
            vec_push(&d->list, id);
            adv();
        } while (accept(P_COMMA));
        expect(P_SEMI, "at the end of the #definePackage directive");
    } else if (strcmp(d->name, "STRICT") == 0 || strcmp(d->name, "strict") == 0) {
        d->name = "STRICT";
        accept(P_SEMI);
    } else if (strcmp(d->name, "NOT_GARBAGE") == 0 || strcmp(d->name, "NOT_GARGBAGE") == 0) {
        d->name = "NOT_GARBAGE";
        accept(P_SEMI);
    } else {
        diag_error(d->loc, "unknown directive '#%s' (valid: #NOT_GARBAGE, #STRICT, #definePackage)", d->name);
    }
    return d;
}

Vec parse_program(TokenList *tl) {
    T = tl->t;
    P = 0;
    g_func_depth = 0;
    Vec items = {0};
    while (K() != TK_EOF) {
        jmp_buf jb;
        jmp_buf *old = g_rec;
        g_rec = &jb;
        volatile int start = P;
        if (setjmp(jb) == 0) {
            Loc loc = LOC();
            g_static = g_abstract = false;
            if (K() == TK_IDENT && !strcmp(cur()->text, "enum") && peekn(1)->kind == TK_IDENT && peekn(2)->kind == P_LBRACE) {
                vec_push(&items, parse_enum());
            } else if (K() == TK_IDENT && !strcmp(cur()->text, "import") && peekn(1)->kind == TK_STRING) {
                Node *d = mk(N_DIRECTIVE, LOC());
                d->name = "import";
                adv();
                d->a = mk(N_STR, LOC());
                d->a->name = cur()->text;
                adv();
                expect(P_SEMI, "at the end of the import");
                vec_push(&items, d);
            } else if (K() == TK_DIRECTIVE) {
                vec_push(&items, parse_directive());
            } else if (K() == KW_PUBLIC || K() == KW_PRIVATE || K() == KW_PROTECTED || K() == KW_PACKAGE || K() == KW_CLASS ||
                       K() == KW_STATIC || K() == KW_ABSTRACT || K() == KW_INTERFACE) {
                int acc = parse_access_mods();
                if (K() == KW_CLASS || K() == KW_INTERFACE) vec_push(&items, parse_class(acc));
                else {
                    Type *ty = parse_type();
                    if (K() == TK_IDENT && peekn(1)->kind == P_LPAREN) vec_push(&items, parse_func_rest(ty, loc, acc));
                    else fail(LOC(), "expected a class or function after the access modifier");
                }
            } else if (type_starts_at(P)) {
                int save = P;
                Type *ty = parse_type();
                if (K() == TK_IDENT && peekn(1)->kind == P_LPAREN) {
                    vec_push(&items, parse_func_rest(ty, loc, ACC_PACKAGE));
                } else {
                    P = save;
                    vec_push(&items, parse_stmt());
                }
            } else {
                vec_push(&items, parse_stmt());
            }
        } else {
            if (P == start) adv();
            int depth = 0;
            while (K() != TK_EOF) {
                if (K() == P_LBRACE) depth++;
                else if (K() == P_RBRACE) { if (depth > 0) { depth--; if (depth == 0) { adv(); break; } } }
                else if (K() == P_SEMI && depth == 0) { adv(); break; }
                adv();
            }
        }
        g_rec = old;
    }
    return items;
}
