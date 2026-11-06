/*
 * codegen.c - Code generator.
 *
 * Convencoes:
 *   - Every expression leaves its result in RAX (double = IEEE bits in RAX).
 *   - Internal calls: arguments pushed in declaration order (the
 *     caller cleans the stack); return in RAX. 'this' is the 1st argument.
 *   - Frame: push rbp / mov rbp,rsp / sub rsp,N. Locais em [rbp - 8*(slot+1)],
 *     parameters at [rbp + 16 + ...]. Slot 0 keeps the return value during 'finally'.
 *   - Windows API calls (the __winapi intrinsic) align the stack to 16
 *     bytes and reserve the shadow space (R12 saves the RSP).
 *   - Exceptions: each 'try' stores a record {previous, rbp, rsp, target} in the
 *     frame e o encadeia em __handler; 'throw' restaura rbp/rsp e salta.
 */
#include "codegen.h"
#include "sema.h"
#include "x64.h"
#include "pe.h"

static Program *P;
static FuncInfo *fn;
static int L_ret, L_null, L_bounds, L_div;

/* ---------------- .data ---------------- */
static uint8_t *data;
static int data_len, data_cap;
typedef struct { char *s; int len; int off; } StrEnt;
static Vec strtab;

static void data_reserve(int n) {
    if (data_len + n > data_cap) {
        data_cap = (data_len + n) * 2 + 256;
        data = xrealloc(data, (size_t)data_cap);
    }
}
static int data_alloc(int n, int align) {
    while (data_len % align) { data_reserve(1); data[data_len++] = 0; }
    data_reserve(n);
    int off = data_len;
    memset(data + off, 0, (size_t)n);
    data_len += n;
    return off;
}
static int data_string(const char *s, int len) {
    for (int i = 0; i < strtab.n; i++) {
        StrEnt *e = strtab.v[i];
        if (e->len == len && memcmp(e->s, s, (size_t)len) == 0) return e->off;
    }
    int off = data_alloc(len + 1, 1);
    memcpy(data + off, s, (size_t)len);
    StrEnt *e = xcalloc(sizeof(StrEnt));
    e->s = xstrndup(s, (size_t)len); e->len = len; e->off = off;
    vec_push(&strtab, e);
    return off;
}

static Symbol *rt_global(const char *name) {
    for (int i = 0; i < P->globals.n; i++)
        if (strcmp(((Symbol *)P->globals.v[i])->name, name) == 0) return P->globals.v[i];
    diag_fatal("internal error: runtime global '%s' not found", name);
    return NULL;
}

/* ---------------- contextos de break/continue/try ---------------- */
enum { CTX_LOOP, CTX_TRY, CTX_SWITCH };
typedef struct { int kind; int brk, cont; int rec_off; Node *fin; } Ctx;
static Ctx ctxs[256];
static int nctx;

/* ---------------- utilidades de emissao ---------------- */
static int this_off(void) { return ((Symbol *)fn->params.v[0])->offset; }
static void null_check(int reg) { a_test_rr(reg, reg); a_jcc(CC_E, L_null); }
static void add_rsp(int bytes) { if (bytes) a_alu_ri(ALU_ADD, RSP, bytes); }
static void call_rt(const char *name, int nargs_on_stack) {
    a_call(rt_func(name)->label);
    add_rsp(8 * nargs_on_stack);
}
static int elem_bytes(Type *t) { return (t->kind == TY_PTR && t->elem->kind == TY_CHAR) || t->kind == TY_PTR_ANY ? 1 : 8; }

static void gen_expr(Node *n);
static void gen_stmt(Node *n);

static void load_sym(Symbol *s) {
    if (s->kind == SYM_GLOBAL) a_load_data(RAX, s->offset);
    else a_load(RAX, RBP, s->offset);
}

/* ---------------- lvalues ---------------- */
typedef enum { LV_LOCAL, LV_GLOBAL, LV_FIELD, LV_ELEM, LV_PTR } LVKind;
typedef struct { LVKind kind; Symbol *sym; int size; bool array; int ebytes; } LV;

static LV lv_prepare(Node *n) {
    LV lv = {0};
    lv.size = 8;
    switch (n->kind) {
    case N_IDENT:
        lv.sym = n->sym;
        if (n->sym->kind == SYM_FIELD) {
            a_load(RAX, RBP, this_off());
            a_push(RAX);
            lv.kind = LV_FIELD;
        } else lv.kind = n->sym->kind == SYM_GLOBAL ? LV_GLOBAL : LV_LOCAL;
        break;
    case N_MEMBER:
        gen_expr(n->a);
        null_check(RAX);
        a_push(RAX);
        lv.kind = LV_FIELD;
        lv.sym = n->sym;
        break;
    case N_INDEX: {
        Type *bt = n->a->type;
        gen_expr(n->a); a_push(RAX);
        gen_expr(n->b); a_push(RAX);
        lv.kind = LV_ELEM;
        lv.array = bt->kind == TY_ARRAY;
        lv.ebytes = bt->kind == TY_STRING ? 1 : bt->kind == TY_ARRAY ? 8 : elem_bytes(bt);
        lv.size = lv.ebytes;
        if (bt->kind == TY_PTR && bt->elem->kind == TY_CHAR) lv.size = 1;
        break;
    }
    case N_UNARY:   /* *p */
        gen_expr(n->a);
        null_check(RAX);
        a_push(RAX);
        lv.kind = LV_PTR;
        lv.size = n->a->type->kind == TY_PTR && n->a->type->elem->kind == TY_CHAR ? 1 : 8;
        break;
    default: diag_fatal("internal error: invalid lvalue");
    }
    return lv;
}

/* lvalue address in RDX (uses RSI) */
static void lv_addr(LV *lv) {
    switch (lv->kind) {
    case LV_LOCAL: a_lea(RDX, RBP, lv->sym->offset); break;
    case LV_GLOBAL: a_lea_data(RDX, lv->sym->offset); break;
    case LV_FIELD: a_load(RDX, RSP, 0); a_lea(RDX, RDX, lv->sym->offset); break;
    case LV_PTR: a_load(RDX, RSP, 0); break;
    case LV_ELEM:
        a_load(RDX, RSP, 0);
        a_load(RSI, RSP, 8);
        null_check(RSI);
        if (lv->array) { a_alu_rm(ALU_CMP, RDX, RSI, 0); a_jcc(CC_AE, L_bounds); }
        if (lv->ebytes == 8) { a_alu_rr(ALU_ADD, RDX, RDX); a_alu_rr(ALU_ADD, RDX, RDX); a_alu_rr(ALU_ADD, RDX, RDX); }
        a_alu_rr(ALU_ADD, RDX, RSI);
        if (lv->array) a_alu_ri(ALU_ADD, RDX, 8);
        break;
    }
}
static void lv_load(LV *lv) {
    lv_addr(lv);
    if (lv->size == 1) a_load8(RAX, RDX, 0); else a_load(RAX, RDX, 0);
}
static void lv_store(LV *lv) {
    lv_addr(lv);
    if (lv->size == 1) a_store8(RDX, 0, RAX); else a_store(RDX, 0, RAX);
}
static void lv_finish(LV *lv) {
    if (lv->kind == LV_FIELD || lv->kind == LV_PTR) add_rsp(8);
    else if (lv->kind == LV_ELEM) add_rsp(16);
}

/* ---------------- aritmetica ---------------- */
static void emit_arith(int op, Type *lt) {   /* RAX = RAX op RCX */
    if (lt->kind == TY_STRING) {             /* only '+' */
        a_push(RAX); a_push(RCX);
        call_rt("__concat", 2);
        return;
    }
    if (lt->kind == TY_PTR || lt->kind == TY_PTR_ANY) {
        if (elem_bytes(lt) == 8) { a_alu_rr(ALU_ADD, RCX, RCX); a_alu_rr(ALU_ADD, RCX, RCX); a_alu_rr(ALU_ADD, RCX, RCX); }
        a_alu_rr((op == P_PLUS || op == P_SHR) ? ALU_ADD : ALU_SUB, RAX, RCX);
        return;
    }
    if (lt->kind == TY_FLOAT) {
        a_movq_xr(XMM0, RAX);
        a_movq_xr(XMM1, RCX);
        int sop = op == P_PLUS ? SSE_ADD : op == P_MINUS ? SSE_SUB : op == P_STAR ? SSE_MUL : SSE_DIV;
        a_sse(sop, XMM0, XMM1);
        a_movq_rx(RAX, XMM0);
        return;
    }
    switch (op) {
    case P_PLUS: a_alu_rr(ALU_ADD, RAX, RCX); break;
    case P_MINUS: a_alu_rr(ALU_SUB, RAX, RCX); break;
    case P_STAR: a_imul_rr(RAX, RCX); break;
    case P_SLASH: case P_PERCENT:
        a_test_rr(RCX, RCX); a_jcc(CC_E, L_div);
        a_cqo(); a_idiv(RCX);
        if (op == P_PERCENT) a_mov_rr(RAX, RDX);
        break;
    case P_AMP: a_alu_rr(ALU_AND, RAX, RCX); break;
    case P_PIPE: a_alu_rr(ALU_OR, RAX, RCX); break;
    case P_CARET: a_alu_rr(ALU_XOR, RAX, RCX); break;
    case P_SHL: a_shift_cl(SH_SHL, RAX); break;
    case P_SHR: a_shift_cl(SH_SAR, RAX); break;
    default: diag_fatal("internal error: arithmetic operator %d", op);
    }
}

static int base_op_of(int op) {
    switch (op) {
    case P_PLUSEQ: return P_PLUS;   case P_MINUSEQ: return P_MINUS;
    case P_STAREQ: return P_STAR;   case P_SLASHEQ: return P_SLASH;
    case P_PERCENTEQ: return P_PERCENT; case P_AMPEQ: return P_AMP;
    case P_PIPEEQ: return P_PIPE;   case P_CARETEQ: return P_CARET;
    case P_SHLEQ: return P_SHL;     case P_SHREQ: return P_SHR;
    default: return 0;
    }
}

static void gen_compare(int op, Type *t) {   /* RAX op RCX -> RAX (0/1) */
    if (t->kind == TY_FLOAT) {
        a_movq_xr(XMM0, RAX);
        a_movq_xr(XMM1, RCX);
        switch (op) {
        case P_LT: a_ucomisd(XMM1, XMM0); a_setcc(CC_A, RAX); break;
        case P_LE: a_ucomisd(XMM1, XMM0); a_setcc(CC_AE, RAX); break;
        case P_GT: a_ucomisd(XMM0, XMM1); a_setcc(CC_A, RAX); break;
        case P_GE: a_ucomisd(XMM0, XMM1); a_setcc(CC_AE, RAX); break;
        case P_EQ: a_ucomisd(XMM0, XMM1); a_setcc(CC_E, RAX); a_setcc(CC_NP, RCX); a_and8_al_cl(); break;
        default:   a_ucomisd(XMM0, XMM1); a_setcc(CC_NE, RAX); a_setcc(CC_P, RCX); a_or8_al_cl(); break;
        }
    } else {
        a_alu_rr(ALU_CMP, RAX, RCX);
        int cc = op == P_EQ ? CC_E : op == P_NE ? CC_NE : op == P_LT ? CC_L : op == P_LE ? CC_LE : op == P_GT ? CC_G : CC_GE;
        a_setcc(cc, RAX);
    }
    a_movzx_r8(RAX, RAX);
}

/* ---------------- excecoes ---------------- */
static void emit_install(int rec_off, int catch_label) {
    a_lea(RAX, RBP, rec_off);
    a_load_data(RCX, rt_global("__handler")->offset);
    a_store(RAX, 0, RCX);
    a_store(RAX, 8, RBP);
    a_store(RAX, 16, RSP);
    a_lea_label(RCX, catch_label);
    a_store(RAX, 24, RCX);
    a_store_data(rt_global("__handler")->offset, RAX);
}
static void emit_pop_handler(int rec_off) {
    a_load(RAX, RBP, rec_off);
    a_store_data(rt_global("__handler")->offset, RAX);
}
static void emit_throw(void) {               /* RAX = exception object */
    a_store_data(rt_global("__exc")->offset, RAX);
    a_load_data(RAX, rt_global("__handler")->offset);
    int Lh = a_label();
    a_test_rr(RAX, RAX);
    a_jcc(CC_NE, Lh);
    a_load_data(RAX, rt_global("__exc")->offset);
    a_push(RAX);
    call_rt("__uncaught", 1);
    a_int3();
    a_bind(Lh);
    a_load(RCX, RAX, 0);
    a_store_data(rt_global("__handler")->offset, RCX);
    a_load(RBP, RAX, 8);
    a_load(RSP, RAX, 16);
    a_jmp_mem(RAX, 24);
}

/* emits the unwinding of contexts down to 'down_to' (try: pop handler + finally) */
static void emit_unwind(int down_to) {
    for (int i = nctx - 1; i >= down_to; i--) {
        if (ctxs[i].kind != CTX_TRY) continue;
        emit_pop_handler(ctxs[i].rec_off);
        if (ctxs[i].fin) {
            Ctx saved[256];
            int sn = nctx;
            memcpy(saved, ctxs, sizeof(Ctx) * (size_t)nctx);
            nctx = i;
            gen_stmt(ctxs[i].fin);
            memcpy(ctxs, saved, sizeof(Ctx) * (size_t)sn);
            nctx = sn;
        }
    }
}
static bool has_finally_between(int down_to) {
    for (int i = nctx - 1; i >= down_to; i--) if (ctxs[i].kind == CTX_TRY && ctxs[i].fin) return true;
    return false;
}

/* ---------------- expressions ---------------- */
static void gen_call(Node *n) {
    int nargs = n->list.n;
    bool recv = n->a != NULL;
    if (recv) { gen_expr(n->a); a_push(RAX); }
    for (int i = 0; i < nargs; i++) { gen_expr(n->list.v[i]); a_push(RAX); }
    FuncInfo *f = n->fn;
    if (recv) {
        a_load(RAX, RSP, 8 * nargs);
        null_check(RAX);
    }
    if ((n->flags & NF_VIRTUAL) && f->owner->is_interface) {      /* interface: looks up the implementation by id */
        a_load(RAX, RAX, 0);
        a_push(RAX);
        a_mov_ri(RAX, f->mid);
        a_push(RAX);
        call_rt("__ifind", 2);
        a_call_reg(RAX);
    } else if (n->flags & NF_VIRTUAL) {
        a_load(RAX, RAX, 0);
        a_call_mem(RAX, 16 + 8 * f->vslot);
    } else a_call(f->label);
    add_rsp(8 * (nargs + (recv ? 1 : 0)));
}

static void gen_builtin(Node *n) {
    switch (n->op) {
    case BI_ARRLEN: gen_expr(n->a); null_check(RAX); a_load(RAX, RAX, 0); break;
    case BI_SQRT:
        gen_expr(n->a);
        a_movq_xr(XMM0, RAX); a_sse(SSE_SQRT, XMM0, XMM0); a_movq_rx(RAX, XMM0);
        break;
    case BI_PEEK8: gen_expr(n->list.v[0]); a_load8(RAX, RAX, 0); break;
    case BI_PEEK64: gen_expr(n->list.v[0]); a_load(RAX, RAX, 0); break;
    case BI_POKE8: case BI_POKE64:
        gen_expr(n->list.v[0]); a_push(RAX);
        gen_expr(n->list.v[1]);
        a_pop(RCX);
        if (n->op == BI_POKE8) a_store8(RCX, 0, RAX); else a_store(RCX, 0, RAX);
        break;
    case BI_SP: a_mov_rr(RAX, RSP); break;
    case BI_GBEGIN: a_lea_data(RAX, 0); break;
    case BI_GEND: a_lea_data(RAX, P->globals_size); break;
    case BI_MEMCPY:
        gen_expr(n->list.v[0]); a_push(RAX);
        gen_expr(n->list.v[1]); a_push(RAX);
        gen_expr(n->list.v[2]);
        a_mov_rr(RCX, RAX);
        a_pop(RSI); a_pop(RDI);
        a_rep_movsb();
        break;
    case BI_WINAPI: {
        int idx = (int)((Node *)n->list.v[0])->ival;
        int na = n->list.n - 1;
        for (int i = 1; i <= na; i++) { gen_expr(n->list.v[i]); a_push(RAX); }
        static const int regs[] = {0, RCX, RDX, R8, R9, R10};
        for (int i = na; i >= 1; i--) a_pop(regs[i]);
        a_mov_rr(R12, RSP);
        a_alu_ri(ALU_SUB, RSP, 48);
        a_alu_ri(ALU_AND, RSP, -16);
        if (na >= 5) a_store(RSP, 32, R10);
        a_call_iat(idx);
        a_mov_rr(RSP, R12);
        break;
    }
    default: diag_fatal("internal error: intrinsic %d", n->op);
    }
}

static void gen_cast(Node *n) {
    Type *from = n->a->type, *to = n->type;
    gen_expr(n->a);
    if (to->kind == TY_FLOAT) {
        if (from->kind != TY_FLOAT) { a_cvtsi2sd(XMM0, RAX); a_movq_rx(RAX, XMM0); }
        return;
    }
    if (from->kind == TY_FLOAT) {
        a_movq_xr(XMM0, RAX);
        a_cvttsd2si(RAX, XMM0);
        if (to->kind == TY_BOOL) { a_test_rr(RAX, RAX); a_setcc(CC_NE, RAX); a_movzx_r8(RAX, RAX); }
        else if (to->kind == TY_CHAR) a_movzx_r8(RAX, RAX);
        return;
    }
    if (to->kind == TY_CHAR) a_movzx_r8(RAX, RAX);
    else if (to->kind == TY_BOOL && from->kind != TY_BOOL) { a_test_rr(RAX, RAX); a_setcc(CC_NE, RAX); a_movzx_r8(RAX, RAX); }
}

static void gen_expr(Node *n) {
    switch (n->kind) {
    case N_INT: case N_CHAR: case N_BOOL: a_mov_ri(RAX, n->ival); break;
    case N_FLOAT: { union { double d; int64_t i; } u; u.d = n->fval; a_mov_ri(RAX, u.i); break; }
    case N_NULL: a_mov_ri(RAX, 0); break;
    case N_STR: a_lea_data(RAX, data_string(n->name, (int)n->ival)); break;
    case N_THIS: a_load(RAX, RBP, this_off()); break;
    case N_IDENT:
        if (!n->sym) diag_fatal("internal error: identifier '%s' (line %d) has no symbol", n->name, n->loc.line);
        if (n->sym->kind == SYM_FIELD) { a_load(RAX, RBP, this_off()); a_load(RAX, RAX, n->sym->offset); }
        else load_sym(n->sym);
        break;
    case N_MEMBER:
        gen_expr(n->a);
        null_check(RAX);
        if (n->flags & NF_ARRLEN) a_load(RAX, RAX, 0);
        else a_load(RAX, RAX, n->sym->offset);
        break;
    case N_INDEX: {
        Type *bt = n->a->type;
        gen_expr(n->a); a_push(RAX);
        gen_expr(n->b);
        a_pop(RCX);
        null_check(RCX);
        int eb = bt->kind == TY_STRING ? 1 : bt->kind == TY_ARRAY ? 8 : elem_bytes(bt);
        if (bt->kind == TY_ARRAY) { a_alu_rm(ALU_CMP, RAX, RCX, 0); a_jcc(CC_AE, L_bounds); }
        if (eb == 8) { a_alu_rr(ALU_ADD, RAX, RAX); a_alu_rr(ALU_ADD, RAX, RAX); a_alu_rr(ALU_ADD, RAX, RAX); }
        a_alu_rr(ALU_ADD, RAX, RCX);
        int disp = bt->kind == TY_ARRAY ? 8 : 0;
        if (eb == 1) a_load8(RAX, RAX, disp); else a_load(RAX, RAX, disp);
        break;
    }
    case N_UNARY:
        if (n->op == P_AMP) {
            LV lv = lv_prepare(n->a);
            lv_addr(&lv);
            a_mov_rr(RAX, RDX);
            lv_finish(&lv);
            break;
        }
        gen_expr(n->a);
        switch (n->op) {
        case P_MINUS:
            if (n->a->type->kind == TY_FLOAT) { a_mov_ri(RCX, (int64_t)0x8000000000000000ULL); a_alu_rr(ALU_XOR, RAX, RCX); }
            else a_neg(RAX);
            break;
        case P_BANG: a_test_rr(RAX, RAX); a_setcc(CC_E, RAX); a_movzx_r8(RAX, RAX); break;
        case P_TILDE: a_not(RAX); break;
        case P_STAR:
            null_check(RAX);
            if (n->a->type->elem->kind == TY_CHAR) a_load8(RAX, RAX, 0); else a_load(RAX, RAX, 0);
            break;
        }
        break;
    case N_BINARY: {
        int op = n->op;
        if (op == P_ANDAND || op == P_OROR) {
            int Lend = a_label(), Lshort = a_label();
            gen_expr(n->a);
            a_test_rr(RAX, RAX);
            a_jcc(op == P_ANDAND ? CC_E : CC_NE, Lshort);
            gen_expr(n->b);
            a_test_rr(RAX, RAX);
            a_setcc(CC_NE, RAX); a_movzx_r8(RAX, RAX);
            a_jmp(Lend);
            a_bind(Lshort);
            a_mov_ri(RAX, op == P_ANDAND ? 0 : 1);
            a_bind(Lend);
            break;
        }
        gen_expr(n->a); a_push(RAX);
        gen_expr(n->b);
        a_mov_rr(RCX, RAX);
        a_pop(RAX);
        if (op == P_EQ || op == P_NE || op == P_LT || op == P_LE || op == P_GT || op == P_GE) gen_compare(op, n->a->type);
        else emit_arith(op, n->a->type);
        break;
    }
    case N_ASSIGN: {
        LV lv = lv_prepare(n->a);
        gen_expr(n->b);
        if (n->op != P_ASSIGN) {
            a_mov_rr(RCX, RAX);
            lv_load(&lv);
            emit_arith(base_op_of(n->op), n->a->type);
        }
        lv_store(&lv);
        lv_finish(&lv);
        break;
    }
    case N_INCDEC: {
        LV lv = lv_prepare(n->a);
        Type *t = n->a->type;
        lv_load(&lv);
        a_mov_rr(R8, RAX);
        if (t->kind == TY_FLOAT) { union { double d; int64_t i; } u; u.d = 1.0; a_mov_ri(RCX, u.i); }
        else a_mov_ri(RCX, 1);
        emit_arith(n->op == P_PLUSPLUS ? P_PLUS : P_MINUS, t);
        lv_store(&lv);
        if (n->flags & NF_POST) a_mov_rr(RAX, R8);
        lv_finish(&lv);
        break;
    }
    case N_CALLFN: gen_call(n); break;
    case N_BUILTIN: gen_builtin(n); break;
    case N_SEQ: for (int i = 0; i < n->list.n; i++) gen_expr(n->list.v[i]); break;
    case N_NEW: {
        ClassInfo *c = n->cls;
        a_mov_ri(RAX, 8 + 8 * c->nfields);
        a_push(RAX);
        call_rt("__alloc", 1);
        a_lea_data(RCX, c->data_off);
        a_store(RAX, 0, RCX);
        a_push(RAX);
        for (int i = 0; i < n->list.n; i++) { gen_expr(n->list.v[i]); a_push(RAX); }
        a_call(n->fn->label);
        add_rsp(8 * (1 + n->list.n));
        break;
    }
    case N_NEWARRAY:
        if (n->b) {                           /* matriz: __newarr2(linhas, colunas) */
            gen_expr(n->a); a_push(RAX);
            gen_expr(n->b); a_push(RAX);
            call_rt("__newarr2", 2);
            break;
        }
        gen_expr(n->a);
        a_test_rr(RAX, RAX);
        a_jcc(CC_S, L_bounds);
        a_push(RAX);                          /* tamanho */
        a_mov_rr(RCX, RAX);
        a_alu_rr(ALU_ADD, RCX, RCX); a_alu_rr(ALU_ADD, RCX, RCX); a_alu_rr(ALU_ADD, RCX, RCX);
        a_alu_ri(ALU_ADD, RCX, 8);
        a_push(RCX);
        call_rt("__alloc", 1);
        a_pop(RCX);
        a_store(RAX, 0, RCX);
        break;
    case N_CAST: gen_cast(n); break;
    case N_INSTANCEOF:
        gen_expr(n->a);
        a_push(RAX);
        a_lea_data(RAX, n->cls->data_off);
        a_push(RAX);
        call_rt("__instanceof", 2);
        break;
    case N_ARRLIT: {
        int cnt = n->list.n;
        a_mov_ri(RAX, 8 * (cnt + 1));
        a_push(RAX);
        call_rt("__alloc", 1);
        a_mov_ri(RCX, cnt);
        a_store(RAX, 0, RCX);
        a_push(RAX);                                  /* mantem o array vivo (GC) durante os elementos */
        for (int i = 0; i < cnt; i++) {
            gen_expr(n->list.v[i]);
            a_load(RCX, RSP, 0);
            a_store(RCX, 8 + 8 * i, RAX);
        }
        a_pop(RAX);
        break;
    }
    case N_COND: {
        int Lelse = a_label(), Lend = a_label();
        gen_expr(n->a);
        a_test_rr(RAX, RAX);
        a_jcc(CC_E, Lelse);
        gen_expr(n->b);
        a_jmp(Lend);
        a_bind(Lelse);
        gen_expr(n->c);
        a_bind(Lend);
        break;
    }
    default: diag_fatal("internal error: gen_expr node %d", n->kind);
    }
}

/* ---------------- statements ---------------- */
static void store_var(Symbol *s) {
    if (s->kind == SYM_GLOBAL) a_store_data(s->offset, RAX);
    else a_store(RBP, s->offset, RAX);
}

static void gen_cond_false(Node *c, int Lfalse) {
    gen_expr(c);
    a_test_rr(RAX, RAX);
    a_jcc(CC_E, Lfalse);
}

static void gen_stmt(Node *n) {
    switch (n->kind) {
    case N_EMPTY: case N_CLASSSTMT: break;
    case N_BLOCK: for (int i = 0; i < n->list.n; i++) gen_stmt(n->list.v[i]); break;
    case N_VAR:
        if (!n->a && n->sym->kind == SYM_GLOBAL) break;      /* .data is already zero */
        if (n->a) gen_expr(n->a); else a_mov_ri(RAX, 0);
        store_var(n->sym);
        break;
    case N_EXPRSTMT: gen_expr(n->a); break;
    case N_IF: {
        int Lelse = a_label(), Lend = a_label();
        gen_cond_false(n->a, Lelse);
        gen_stmt(n->b);
        if (n->c) a_jmp(Lend);
        a_bind(Lelse);
        if (n->c) { gen_stmt(n->c); a_bind(Lend); }
        break;
    }
    case N_WHILE: {
        int Ltop = a_label(), Lend = a_label();
        a_bind(Ltop);
        gen_cond_false(n->a, Lend);
        ctxs[nctx++] = (Ctx){CTX_LOOP, Lend, Ltop, 0, NULL};
        gen_stmt(n->b);
        nctx--;
        a_jmp(Ltop);
        a_bind(Lend);
        break;
    }
    case N_DO: {
        int Ltop = a_label(), Lcont = a_label(), Lend = a_label();
        a_bind(Ltop);
        ctxs[nctx++] = (Ctx){CTX_LOOP, Lend, Lcont, 0, NULL};
        gen_stmt(n->a);
        nctx--;
        a_bind(Lcont);
        gen_expr(n->b);
        a_test_rr(RAX, RAX);
        a_jcc(CC_NE, Ltop);
        a_bind(Lend);
        break;
    }
    case N_FOR: {
        int Ltop = a_label(), Lcont = a_label(), Lend = a_label();
        if (n->a) gen_stmt(n->a);
        a_bind(Ltop);
        if (n->b) gen_cond_false(n->b, Lend);
        ctxs[nctx++] = (Ctx){CTX_LOOP, Lend, Lcont, 0, NULL};
        gen_stmt(n->d);
        nctx--;
        a_bind(Lcont);
        if (n->c) gen_expr(n->c);
        a_jmp(Ltop);
        a_bind(Lend);
        break;
    }
    case N_RETURN: {
        if (n->a) gen_expr(n->a);
        bool fin = has_finally_between(0);
        if (fin) a_store(RBP, -8, RAX);
        emit_unwind(0);
        if (fin) a_load(RAX, RBP, -8);
        a_jmp(L_ret);
        break;
    }
    case N_BREAK: case N_CONTINUE: {
        int i = nctx - 1;
        while (i >= 0 && ctxs[i].kind != CTX_LOOP && !(n->kind == N_BREAK && ctxs[i].kind == CTX_SWITCH)) i--;
        emit_unwind(i + 1);
        a_jmp(n->kind == N_BREAK ? ctxs[i].brk : ctxs[i].cont);
        break;
    }
    case N_SWITCH: {
        int off = -8 * (n->slot + 1);
        int ncase = n->list.n;
        int *lab = xcalloc(sizeof(int) * (size_t)(ncase + 1));
        int Lend = a_label(), Ldef = -1;
        bool isstr = n->a->type->kind == TY_STRING;
        gen_expr(n->a);
        a_store(RBP, off, RAX);
        for (int i = 0; i < ncase; i++) {
            Node *cs = n->list.v[i];
            lab[i] = a_label();
            if (!cs->a) { Ldef = lab[i]; continue; }
            if (isstr) {
                a_load(RAX, RBP, off); a_push(RAX);
                a_lea_data(RAX, data_string(cs->a->name, (int)cs->a->ival)); a_push(RAX);
                call_rt("__streq", 2);
                a_test_rr(RAX, RAX);
                a_jcc(CC_NE, lab[i]);
            } else {
                a_load(RAX, RBP, off);
                a_mov_ri(RCX, cs->ival);
                a_alu_rr(ALU_CMP, RAX, RCX);
                a_jcc(CC_E, lab[i]);
            }
        }
        a_jmp(Ldef >= 0 ? Ldef : Lend);
        ctxs[nctx++] = (Ctx){CTX_SWITCH, Lend, 0, 0, NULL};
        for (int i = 0; i < ncase; i++) {
            Node *cs = n->list.v[i];
            a_bind(lab[i]);
            for (int k = 0; k < cs->list.n; k++) gen_stmt(cs->list.v[k]);
        }
        nctx--;
        a_bind(Lend);
        break;
    }
    case N_THROW:
        gen_expr(n->a);
        null_check(RAX);
        emit_throw();
        break;
    case N_TRY: {
        int rec = -8 * (n->slot + 4);
        int Lcatch = a_label(), Lfin_n = a_label(), Lrethrow = a_label();
        emit_install(rec, Lcatch);
        ctxs[nctx++] = (Ctx){CTX_TRY, 0, 0, rec, n->b};
        gen_stmt(n->a);
        nctx--;
        emit_pop_handler(rec);
        a_jmp(Lfin_n);
        a_bind(Lcatch);
        for (int i = 0; i < n->list.n; i++) {
            Node *c = n->list.v[i];
            int Lnext = a_label();
            a_load_data(RAX, rt_global("__exc")->offset);
            a_push(RAX);
            a_lea_data(RAX, c->type->cls->data_off);
            a_push(RAX);
            call_rt("__instanceof", 2);
            a_test_rr(RAX, RAX);
            a_jcc(CC_E, Lnext);
            a_load_data(RAX, rt_global("__exc")->offset);
            a_store(RBP, c->sym->offset, RAX);
            if (n->b) {
                emit_install(rec, Lrethrow);
                ctxs[nctx++] = (Ctx){CTX_TRY, 0, 0, rec, n->b};
            }
            gen_stmt(c->a);
            if (n->b) { nctx--; emit_pop_handler(rec); }
            a_jmp(Lfin_n);
            a_bind(Lnext);
        }
        a_bind(Lrethrow);
        if (n->b) gen_stmt(n->b);
        a_load_data(RAX, rt_global("__exc")->offset);
        emit_throw();
        a_bind(Lfin_n);
        if (n->b) gen_stmt(n->b);
        break;
    }
    default: diag_fatal("internal error: gen_stmt node %d", n->kind);
    }
}

/* ---------------- functions ---------------- */
static void gen_func(FuncInfo *f) {
    fn = f;
    nctx = 0;
    L_ret = a_label();
    a_bind(f->label);
    a_push(RBP);
    a_mov_rr(RBP, RSP);
    int patch = a_sub_rsp_patchable();

    int first = 0;
    if (f->is_ctor) {
        ClassInfo *c = f->owner;
        Node *body = f->body;
        bool explicit_super = false;
        if (body && body->list.n > 0) {
            Node *s0 = body->list.v[0];
            if (s0->kind == N_EXPRSTMT && s0->a->kind == N_CALLFN && (s0->a->flags & NF_SUPERCALL) && s0->a->fn->is_ctor) explicit_super = true;
        }
        if (explicit_super) { gen_stmt(body->list.v[0]); first = 1; }
        else if (c->parent) {
            FuncInfo *pc = NULL;
            for (int i = 0; i < c->parent->ctors.n; i++) if (((FuncInfo *)c->parent->ctors.v[i])->nargs == 0) pc = c->parent->ctors.v[i];
            a_load(RAX, RBP, this_off());
            a_push(RAX);
            a_call(pc->label);
            add_rsp(8);
        }
        for (int i = 0; i < c->decl->list.n; i++) {
            Node *m = c->decl->list.v[i];
            if (m->kind != N_FIELD || !m->a || !m->sym) continue;
            gen_expr(m->a);
            a_load(RCX, RBP, this_off());
            a_store(RCX, m->sym->offset, RAX);
        }
    }
    if (f->body) {
        if (f->is_ctor || f->body->kind != N_BLOCK) {
            for (int i = first; i < f->body->list.n; i++) gen_stmt(f->body->list.v[i]);
        } else gen_stmt(f->body);
    }
    a_bind(L_ret);
    if (f->is_ctor) a_load(RAX, RBP, this_off());
    a_leave();
    a_ret();
    int frame = 8 * (f->frame_slots + 1);
    frame = (frame + 15) & ~15;
    a_patch32(patch, frame);
}

void codegen_emit(Program *prog, const char *out_path) {
    P = prog;
    /* labels of all functions */
    for (int i = 0; i < P->funcs.n; i++) ((FuncInfo *)P->funcs.v[i])->label = a_label();
    L_null = a_label(); L_bounds = a_label(); L_div = a_label();
    int L_entry = a_label();

    /* .data: globals, then vtables, then strings */
    data_alloc(P->globals_size ? P->globals_size : 8, 8);
    for (int i = 0; i < P->classes.n; i++) {
        ClassInfo *c = P->classes.v[i];
        c->data_off = data_alloc(8 * (2 + c->vtable.n), 8);     /* [parent][itable][methods...] */
    }
    for (int i = 0; i < P->classes.n; i++) {
        ClassInfo *c = P->classes.v[i];
        if (c->parent) a_data_fix_func(c->data_off, -1 - c->parent->data_off);
        for (int j = 0; j < c->vtable.n; j++) a_data_fix_func(c->data_off + 8 * (2 + j), ((FuncInfo *)c->vtable.v[j])->label);
        /* itable: [n][id, function]*  used by calls through an interface */
        int it = data_alloc(8 + 16 * c->vtable.n, 8);
        for (int k = 0; k < 8; k++) data[it + k] = (uint8_t)((int64_t)c->vtable.n >> (8 * k));
        for (int j = 0; j < c->vtable.n; j++) {
            FuncInfo *f = c->vtable.v[j];
            for (int k = 0; k < 8; k++) data[it + 8 + 16 * j + k] = (uint8_t)((int64_t)f->mid >> (8 * k));
            a_data_fix_func(it + 16 + 16 * j, f->label);
        }
        a_data_fix_func(c->data_off + 8, -1 - it);
    }

    /* ponto de entrada */
    a_bind(L_entry);
    a_push(RBP);
    a_mov_rr(RBP, RSP);
    a_store_data(rt_global("__stack_base")->offset, RSP);
    a_call(rt_func("__setup")->label);
    a_call(P->globals_init->label);
    a_call(P->main_fn->label);
    if (P->main_fn->ret->kind == TY_VOID) a_mov_ri(RAX, 0);
    a_push(RAX);
    call_rt("__exit", 1);
    a_int3();

    for (int i = 0; i < P->funcs.n; i++) gen_func(P->funcs.v[i]);

    a_bind(L_null);   a_call(rt_func("__panic_null")->label);
    a_bind(L_bounds); a_call(rt_func("__panic_bounds")->label);
    a_bind(L_div);    a_call(rt_func("__panic_div")->label);

    pe_write(out_path, data, data_len, L_entry);
}
