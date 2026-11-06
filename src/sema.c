/*
 * sema.c - Semantic analysis.
 *
 * Fases:
 *   1. declare classes, order them by inheritance, build fields/methods/vtables
 *   2. declare global functions
 *   3. analyze the top-level statements (they become the synthetic __globals_init function)
 *   4. analyze the bodies of functions, methods and constructors
 *
 * During the analysis each expression gets a 'type', identifiers get a
 * 'sym', conversoes implicitas viram N_CAST e print/printf/strings viram
 * direct calls to the runtime functions (the .adac files in runtime/).
 */
#include "sema.h"

/* ====================== estado global ====================== */
typedef struct Scope {
    Vec syms;
    struct Scope *parent;
    int depth;
} Scope;

static Program *prog;
static Vec all_classes;          /* ClassInfo* in declaration order */
static Vec all_funcs;            /* FuncInfo* globais (livres) */
static Scope *global_scope;
static Scope *scope;
static FuncInfo *cur_fn;
static ClassInfo *cur_cls;
static int next_slot;
static int loop_depth;
static int switch_depth;
static Vec implicit_syms;
static Vec static_inits;         /* N_VAR with preset sym: static field initializers */
static Vec all_enums;            /* Node* N_ENUM */
static Node *find_enum(const char *name) {
    for (int i = 0; i < all_enums.n; i++) if (!strcmp(((Node *)all_enums.v[i])->name, name)) return all_enums.v[i];
    return NULL;
}
static Type *cur_ret;
static bool in_global_init;
static ClassInfo *exception_cls;
static Node *allowed_super;      /* the only allowed super(...) node (1st statement of the ctor) */

/* ====================== utilidades ====================== */
static bool in_prelude(Loc l) { return l.src && l.src->is_prelude; }

static ClassInfo *find_class(const char *name) {
    for (int i = 0; i < all_classes.n; i++)
        if (strcmp(((ClassInfo *)all_classes.v[i])->name, name) == 0) return all_classes.v[i];
    return NULL;
}
static bool implements_iface(ClassInfo *c, ClassInfo *iface) {
    for (; c; c = c->parent) {
        if (c == iface) return true;
        for (int i = 0; i < c->ifaces.n; i++) if (implements_iface(c->ifaces.v[i], iface)) return true;
    }
    return false;
}
static bool is_subclass(ClassInfo *c, ClassInfo *base) {
    if (base->is_interface) return implements_iface(c, base);
    for (; c; c = c->parent) if (c == base) return true;
    return false;
}
static Symbol *find_field(ClassInfo *c, const char *name) {
    for (; c; c = c->parent)
        for (int i = 0; i < c->fields.n; i++)
            if (strcmp(((Symbol *)c->fields.v[i])->name, name) == 0) return c->fields.v[i];
    return NULL;
}
static FuncInfo *find_method(ClassInfo *c, const char *name, int nargs) {
    for (ClassInfo *k = c; k; k = k->parent)
        for (int i = 0; i < k->methods.n; i++) {
            FuncInfo *f = k->methods.v[i];
            if (f->nargs == nargs && strcmp(f->name, name) == 0) return f;
        }
    for (ClassInfo *k = c; k; k = k->parent)
        for (int i = 0; i < k->ifaces.n; i++) {
            FuncInfo *f = find_method(k->ifaces.v[i], name, nargs);
            if (f) return f;
        }
    return NULL;
}
static bool has_method_named(ClassInfo *c, const char *name) {
    for (ClassInfo *k = c; k; k = k->parent) {
        for (int i = 0; i < k->methods.n; i++)
            if (strcmp(((FuncInfo *)k->methods.v[i])->name, name) == 0) return true;
        for (int i = 0; i < k->ifaces.n; i++) if (has_method_named(k->ifaces.v[i], name)) return true;
    }
    return false;
}
static Symbol *find_static(ClassInfo *c, const char *name) {
    for (; c; c = c->parent)
        for (int i = 0; i < c->statics.n; i++)
            if (!strcmp(((Symbol *)c->statics.v[i])->name, name)) return c->statics.v[i];
    return NULL;
}
static Vec mid_keys;
static int method_id(const char *name, int nargs) {
    char *k = xsprintf("%s/%d", name, nargs);
    for (int i = 0; i < mid_keys.n; i++) if (!strcmp(mid_keys.v[i], k)) return i + 1;
    vec_push(&mid_keys, k);
    return mid_keys.n;
}
static FuncInfo *find_function(const char *name, int nargs) {
    for (int i = 0; i < all_funcs.n; i++) {
        FuncInfo *f = all_funcs.v[i];
        if (f->nargs == nargs && strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}
static bool has_function_named(const char *name) {
    for (int i = 0; i < all_funcs.n; i++)
        if (strcmp(((FuncInfo *)all_funcs.v[i])->name, name) == 0) return true;
    return false;
}

FuncInfo *rt_func(const char *name) {
    for (int i = 0; i < all_funcs.n; i++) {
        FuncInfo *f = all_funcs.v[i];
        if (f->is_prelude && strcmp(f->name, name) == 0) return f;
    }
    diag_fatal("internal error: runtime function '%s' not found", name);
    return NULL;
}

static bool access_ok(int access, ClassInfo *owner) {
    if (access == ACC_PRIVATE) return cur_cls == owner;
    if (access == ACC_PROTECTED) return cur_cls && is_subclass(cur_cls, owner);
    return true;
}
static void check_access(int access, ClassInfo *owner, Loc loc, const char *kind, const char *name) {
    if (access_ok(access, owner)) return;
    if (access == ACC_PRIVATE) diag_error(loc, "%s '%s' is private in '%s' and cannot be accessed here", kind, name, owner->name);
    else diag_error(loc, "%s '%s' is protected in '%s' and can only be accessed by subclasses", kind, name, owner->name);
}

/* ====================== scopes ====================== */
static void push_scope(void) {
    Scope *s = xcalloc(sizeof(Scope));
    s->parent = scope;
    s->depth = scope ? scope->depth + 1 : 0;
    scope = s;
}
static void pop_scope(void) { scope = scope->parent; }

static Symbol *scope_find_here(Scope *s, const char *name) {
    for (int i = s->syms.n - 1; i >= 0; i--)
        if (strcmp(((Symbol *)s->syms.v[i])->name, name) == 0) return s->syms.v[i];
    return NULL;
}

static Symbol *lookup_var(const char *name) {
    for (Scope *s = scope; s; s = s->parent) {
        Symbol *sym = scope_find_here(s, name);
        if (sym) return sym;
    }
    return scope_find_here(global_scope, name);
}

static int alloc_slot(void) {
    int s = next_slot++;
    if (cur_fn && next_slot > cur_fn->frame_slots) cur_fn->frame_slots = next_slot;
    return s;
}

static Symbol *declare_var(const char *name, Type *type, bool is_const, Loc loc) {
    Symbol *prev = scope_find_here(scope, name);
    if (prev) {
        diag_error(loc, "'%s' is already declared in this scope (line %d)", name, prev->loc.line);
        return prev;
    }
    Symbol *s = xcalloc(sizeof(Symbol));
    s->name = (char *)name;
    s->type = type;
    s->is_const = is_const;
    s->loc = loc;
    if (in_global_init && scope->depth == 0) {          /* global variable */
        s->kind = SYM_GLOBAL;
        s->offset = prog->globals_size;
        prog->globals_size += 8;
        vec_push(&prog->globals, s);
        vec_push(&global_scope->syms, s);
        return s;
    }
    s->kind = SYM_LOCAL;
    s->offset = -8 * (alloc_slot() + 1);
    vec_push(&scope->syms, s);
    return s;
}

/* ====================== types ====================== */
static Type *resolve_type(Type *t) {
    if (!t) return t;
    switch (t->kind) {
    case TY_CLASS:
        if (!t->cls) {
            t->cls = find_class(t->name);
            if (t->cls && t->cls->is_prelude && !t->elem && (!strcmp(t->name, "List") || !strcmp(t->name, "Map"))) {
                diag_error(t->loc, "'%s' needs an element type: %s", t->name, !strcmp(t->name, "List") ? "List<int>" : "Map<String, int>");
                return TY_error_t;
            }
            if (t->cls && t->elem) { t->elem = resolve_type(t->elem); if (t->elem == TY_error_t) return TY_error_t; }
            if (!t->cls && find_enum(t->name)) return TY_int_t;       /* an enum is an int with names */
            if (!t->cls) {
                diag_error(t->loc, "unknown type '%s' (class not declared)", t->name);
                return TY_error_t;
            }
        }
        return t;
    case TY_PTR: case TY_ARRAY: {
        Type *e = resolve_type(t->elem);
        if (e == TY_error_t) return TY_error_t;
        t->elem = e;
        return t;
    }
    default: return t;
    }
}

static bool is_err(Type *t) { return t == TY_error_t || t->kind == TY_ERROR; }

/* "condition" type: accepts bool, int, char and references */
static bool cond_type_ok(Type *t) {
    return t->kind == TY_BOOL || t->kind == TY_INT || t->kind == TY_CHAR || type_is_reference(t);
}

static int __attribute__((unused)) elem_size(Type *t) {           /* size of what a T* pointer points to */
    if (t->kind == TY_PTR) return t->elem->kind == TY_CHAR ? 1 : 8;
    return 1;                              /* ptr generico: bytes */
}

/* ====================== helper nodes ====================== */
static Node *mk_int(Loc l, int64_t v) { Node *n = node_new(N_INT, l); n->ival = v; n->type = TY_int_t; return n; }
static Node *mk_str(Loc l, const char *s, int len) {
    Node *n = node_new(N_STR, l);
    n->name = xstrndup(s, (size_t)len);
    n->ival = len;
    n->type = TY_string_t;
    return n;
}
static Node *mk_callfn(Loc l, FuncInfo *f, Node *a, Node *b, Node *c) {
    Node *n = node_new(N_CALLFN, l);
    n->fn = f;
    n->type = f->ret ? f->ret : TY_void_t;
    if (a) vec_push(&n->list, a);
    if (b) vec_push(&n->list, b);
    if (c) vec_push(&n->list, c);
    return n;
}
static Node *mk_rt(Loc l, const char *name, Node *a, Node *b, Node *c) { return mk_callfn(l, rt_func(name), a, b, c); }

static Node *mk_cast(Node *e, Type *to, bool implicit) {
    Node *c = node_new(N_CAST, e->loc);
    c->a = e;
    c->type = to;
    if (implicit) c->flags |= NF_IMPLICIT;
    return c;
}

/* converts any expression to String */
static Node *to_string_node(Node *e) {
    Type *t = e->type;
    switch (t->kind) {
    case TY_STRING: return e;
    case TY_NULL: return mk_str(e->loc, "null", 4);
    case TY_INT: return mk_rt(e->loc, "__itoa", e, NULL, NULL);
    case TY_CHAR: return mk_rt(e->loc, "__ctoa", e, NULL, NULL);
    case TY_BOOL: return mk_rt(e->loc, "__btoa", e, NULL, NULL);
    case TY_FLOAT: return mk_rt(e->loc, "__ftoa", e, mk_int(e->loc, -1), NULL);
    case TY_CLASS: case TY_ARRAY: case TY_PTR: case TY_PTR_ANY: return mk_rt(e->loc, "__ptoa", e, NULL, NULL);
    default:
        diag_error(e->loc, "cannot convert '%s' to String", type_str(t));
        return e;
    }
}

/*
 * Checks whether 'e' (type from) can be assigned to 'to' and returns the node
 * already converted (inserting N_CAST when needed). Reports an error otherwise.
 */
static Node *coerce(Node *e, Type *to, const char *ctx) {
    Type *from = e->type;
    if (is_err(from) || is_err(to)) return e;
    if (from->kind == TY_VOID) {
        diag_error(e->loc, "%s: the expression produces no value (type void)", ctx);
        return e;
    }
    if (type_equal(from, to)) return e;
    if (from->kind == TY_CLASS && to->kind == TY_CLASS && from->cls == to->cls && (from->elem || to->elem)) {
        diag_error(e->loc, "%s: cannot convert '%s' to '%s' (different element types)", ctx, type_str(from), type_str(to));
        return e;
    }
    switch (to->kind) {
    case TY_FLOAT:
        if (from->kind == TY_INT || from->kind == TY_CHAR || from->kind == TY_BOOL) return mk_cast(e, to, true);
        break;
    case TY_INT:
        if (from->kind == TY_CHAR || from->kind == TY_BOOL) return e;
        break;
    case TY_CHAR:
        if (from->kind == TY_INT) return mk_cast(e, to, true);
        if (from->kind == TY_BOOL) return e;
        break;
    case TY_BOOL: break;
    case TY_STRING: case TY_ARRAY: case TY_CLASS:
        if (from->kind == TY_NULL) { e->type = to; return e; }
        if (to->kind == TY_CLASS && from->kind == TY_CLASS && is_subclass(from->cls, to->cls)) return e;
        break;
    case TY_PTR: case TY_PTR_ANY:
        if (from->kind == TY_NULL) { e->type = to; return e; }
        if (from->kind == TY_PTR_ANY || (to->kind == TY_PTR_ANY && from->kind == TY_PTR)) return e;
        break;
    default: break;
    }
    const char *hint = "";
    if (to->kind == TY_INT && from->kind == TY_FLOAT) hint = " (use an explicit cast: (int) value)";
    else if (to->kind == TY_CLASS && from->kind == TY_CLASS && is_subclass(to->cls, from->cls)) hint = " (use an explicit cast to the subclass)";
    diag_error(e->loc, "%s: cannot convert '%s' to '%s'%s", ctx, type_str(from), type_str(to), hint);
    return e;
}

/* ====================== classes ====================== */
static FuncInfo *new_func(const char *name, Type *ret, ClassInfo *owner, Loc loc) {
    FuncInfo *f = xcalloc(sizeof(FuncInfo));
    f->name = (char *)name;
    f->ret = ret ? ret : TY_void_t;
    f->owner = owner;
    f->loc = loc;
    f->is_prelude = in_prelude(loc);
    f->vslot = -1;
    return f;
}

static void add_param(FuncInfo *f, const char *name, Type *t, Loc loc) {
    Symbol *s = xcalloc(sizeof(Symbol));
    s->kind = SYM_PARAM;
    s->name = (char *)name;
    s->type = t;
    s->loc = loc;
    vec_push(&f->params, s);
    f->nparams++;
}

/* parameter offsets: the 1st one pushed sits at the highest address */
static void finalize_params(FuncInfo *f) {
    for (int i = 0; i < f->params.n; i++) ((Symbol *)f->params.v[i])->offset = 16 + 8 * (f->nparams - 1 - i);
}

static FuncInfo *build_func(Node *d, ClassInfo *owner) {
    bool is_ctor = (d->flags & NF_CTOR) != 0;
    Type *ret = d->type ? resolve_type(d->type) : TY_void_t;
    FuncInfo *f = new_func(d->name, ret, owner, d->loc);
    f->decl = d;
    f->body = d->a;
    f->access = d->access;
    f->is_ctor = is_ctor;
    bool is_static = (d->flags & NF_STATIC) != 0;
    f->is_static = is_static;
    f->is_abstract = (d->flags & NF_ABSTRACT) != 0;
    if (owner && !is_static) add_param(f, "this", type_class_of(owner), d->loc);
    for (int i = 0; i < d->list.n; i++) {
        Node *p = d->list.v[i];
        Type *pt = resolve_type(p->type);
        if (pt->kind == TY_VOID) { diag_error(p->loc, "parameter '%s' cannot be void", p->name); pt = TY_error_t; }
        for (int j = (owner && !is_static) ? 1 : 0; j < f->params.n; j++)
            if (strcmp(((Symbol *)f->params.v[j])->name, p->name) == 0) diag_error(p->loc, "duplicate parameter '%s'", p->name);
        add_param(f, p->name, pt, p->loc);
    }
    f->nargs = f->nparams - ((owner && !is_static) ? 1 : 0);
    finalize_params(f);
    f->is_virtual = owner && !is_ctor && !is_static && f->access != ACC_PRIVATE;
    if (f->is_virtual) f->mid = method_id(f->name, f->nargs);
    return f;
}

static bool same_signature(FuncInfo *a, FuncInfo *b) {
    if (a->nargs != b->nargs || !type_equal(a->ret, b->ret)) return false;
    for (int i = 1; i < a->params.n; i++)
        if (!type_equal(((Symbol *)a->params.v[i])->type, ((Symbol *)b->params.v[i])->type)) return false;
    return true;
}

static void declare_classes(Vec *items) {
    for (int i = 0; i < items->n; i++) {
        Node *d = items->v[i];
        if (d->kind != N_CLASS) continue;
        if (find_class(d->name)) {
            diag_error(d->loc, "class '%s' is already declared", d->name);
            continue;
        }
        if (!in_prelude(d->loc) && strncmp(d->name, "__", 2) == 0)
            diag_error(d->loc, "names starting with '__' are reserved for the runtime");
        ClassInfo *c = xcalloc(sizeof(ClassInfo));
        c->name = d->name;
        c->decl = d;
        c->loc = d->loc;
        c->is_prelude = in_prelude(d->loc);
        c->is_abstract = (d->flags & NF_ABSTRACT) != 0;
        c->is_interface = (d->flags & NF_INTERFACE) != 0;
        d->cls = c;
        vec_push(&all_classes, c);
    }
}

static void link_parents(void) {
    for (int i = 0; i < all_classes.n; i++) {
        ClassInfo *c = all_classes.v[i];
        Node *pn = c->decl->d;
        if (!pn) continue;
        ClassInfo *p = find_class(pn->name);
        if (!p) { diag_error(pn->loc, "superclass '%s' is not declared", pn->name); continue; }
        if (p == c) { diag_error(pn->loc, "class '%s' cannot inherit from itself", c->name); continue; }
        if (p->is_interface) { diag_error(pn->loc, "'%s' is an interface: use 'implements' instead of 'extends'", p->name); continue; }
        c->parent = p;
    }
    for (int i = 0; i < all_classes.n; i++) {                 /* interfaces implementadas/estendidas */
        ClassInfo *c = all_classes.v[i];
        if (!c->decl->a) continue;
        for (int j = 0; j < c->decl->a->list.n; j++) {
            Node *id = c->decl->a->list.v[j];
            ClassInfo *f = find_class(id->name);
            if (!f) { diag_error(id->loc, "interface '%s' is not declared", id->name); continue; }
            if (!f->is_interface) { diag_error(id->loc, "'%s' is not an interface (use 'extends' to inherit from classes)", id->name); continue; }
            if (f == c) { diag_error(id->loc, "interface '%s' cannot extend itself", c->name); continue; }
            vec_push(&c->ifaces, f);
        }
    }
    /* detecta ciclos */
    for (int i = 0; i < all_classes.n; i++) {
        ClassInfo *c = all_classes.v[i];
        int steps = 0;
        for (ClassInfo *p = c->parent; p; p = p->parent)
            if (p == c || ++steps > all_classes.n) {
                diag_error(c->loc, "circular inheritance involving class '%s'", c->name);
                c->parent = NULL;
                break;
            }
    }
}

static void setup_class(ClassInfo *c) {
    if (c->resolved) return;
    c->resolved = true;
    if (c->parent) {
        setup_class(c->parent);
        c->nfields = c->parent->nfields;
        for (int i = 0; i < c->parent->vtable.n; i++) vec_push(&c->vtable, c->parent->vtable.v[i]);
    }
    ClassInfo *saved = cur_cls;
    cur_cls = c;
    Node *cd = c->decl;
    /* fields */
    for (int i = 0; i < cd->list.n; i++) {
        Node *m = cd->list.v[i];
        if (m->kind != N_FIELD) continue;
        Type *t = resolve_type(m->type);
        if (t->kind == TY_VOID) { diag_error(m->loc, "field '%s' cannot be void", m->name); t = TY_error_t; }
        Symbol *dup = find_field(c, m->name);
        if (dup) {
            diag_error(m->loc, "field '%s' is already declared%s (line %d)", m->name, dup->owner == c ? "" : " in the superclass", dup->loc.line);
            continue;
        }
        if (c->is_interface) { diag_error(m->loc, "interfaces cannot have fields ('%s')", m->name); continue; }
        Symbol *s = xcalloc(sizeof(Symbol));
        s->name = m->name;
        s->type = t;
        s->access = m->access;
        s->owner = c;
        s->loc = m->loc;
        m->sym = s;
        if (m->flags & NF_STATIC) {                    /* static field = global named after the class */
            s->kind = SYM_GLOBAL;
            s->offset = -1;                              /* set at the end of the analysis (avoids clashing with other globals) */
            vec_push(&prog->globals, s);
            vec_push(&c->statics, s);
            if (m->a) { Node *iv = node_new(N_VAR, m->loc); iv->sym = s; iv->type = t; iv->a = m->a; iv->cls = c; iv->flags |= NF_GLOBALINIT; vec_push(&static_inits, iv); }
            continue;
        }
        s->kind = SYM_FIELD;
        s->offset = 8 + 8 * c->nfields++;
        vec_push(&c->fields, s);
    }
    /* methods and constructors */
    for (int i = 0; i < cd->list.n; i++) {
        Node *m = cd->list.v[i];
        if (m->kind != N_FUNC) continue;
        FuncInfo *f = build_func(m, c);
        m->fn = f;
        if (c->is_interface) {
            if (f->is_ctor || f->is_static || !(m->flags & NF_ABSTRACT)) diag_error(m->loc, "interfaces can only declare instance methods without a body ('%s')", f->name);
            f->is_abstract = true;
            f->access = ACC_PUBLIC;
        } else if (f->is_abstract && !c->is_abstract && !f->is_static) {
            diag_error(m->loc, "abstract method '%s' requires class '%s' to be declared 'abstract'", f->name, c->name);
        }
        Vec *dst = f->is_ctor ? &c->ctors : &c->methods;
        FuncInfo *dup = NULL;
        for (int j = 0; j < dst->n; j++) {
            FuncInfo *o = dst->v[j];
            if (o->nargs == f->nargs && strcmp(o->name, f->name) == 0) dup = o;
        }
        if (dup) {
            diag_error(m->loc, "%s '%s' with %d parameter(s) is already declared in this class (line %d)",
                       f->is_ctor ? "constructor" : "method", f->name, f->nargs, dup->loc.line);
            diag_note(dup->loc, "previous declaration; overloads are distinguished only by the number of parameters");
            continue;
        }
        vec_push(dst, f);
        vec_push(&prog->funcs, f);
    }
    /* construtor padrao sintetico */
    if (c->ctors.n == 0) {
        FuncInfo *f = new_func(c->name, TY_void_t, c, c->loc);
        f->is_ctor = true;
        add_param(f, "this", type_class_of(c), c->loc);
        finalize_params(f);
        vec_push(&c->ctors, f);
        vec_push(&prog->funcs, f);
    }
    /* vtable (interfaces have none: dispatch by id) */
    for (int i = 0; i < c->methods.n; i++) {
        FuncInfo *f = c->methods.v[i];
        if (!f->is_virtual || c->is_interface) continue;
        int slot = -1;
        for (int j = 0; j < c->vtable.n; j++) {
            FuncInfo *o = c->vtable.v[j];
            if (o->nargs == f->nargs && strcmp(o->name, f->name) == 0) { slot = j; break; }
        }
        if (slot >= 0) {
            FuncInfo *o = c->vtable.v[slot];
            if (!same_signature(f, o))
                diag_error(f->loc, "'%s' overrides the method of '%s' with an incompatible signature (different return/parameter types)", f->name, o->owner->name);
            f->vslot = slot;
            c->vtable.v[slot] = f;
        } else {
            f->vslot = c->vtable.n;
            vec_push(&c->vtable, f);
        }
    }
    cur_cls = saved;
}

static void collect_iface_methods(ClassInfo *iface, Vec *out) {
    for (int i = 0; i < iface->methods.n; i++) vec_push(out, iface->methods.v[i]);
    for (int i = 0; i < iface->ifaces.n; i++) collect_iface_methods(iface->ifaces.v[i], out);
}
static void check_contracts(ClassInfo *c) {
    if (c->is_abstract || c->is_interface) return;
    for (int i = 0; i < c->vtable.n; i++) {
        FuncInfo *f = c->vtable.v[i];
        if (f->is_abstract) diag_error(c->loc, "class '%s' does not implement abstract method '%s' (declared in '%s')", c->name, f->name, f->owner->name);
    }
    Vec need = {0};
    for (ClassInfo *k = c; k; k = k->parent)
        for (int i = 0; i < k->ifaces.n; i++) collect_iface_methods(k->ifaces.v[i], &need);
    for (int i = 0; i < need.n; i++) {
        FuncInfo *im = need.v[i];
        FuncInfo *impl = NULL;
        for (int j = 0; j < c->vtable.n; j++) {
            FuncInfo *f = c->vtable.v[j];
            if (f->nargs == im->nargs && !strcmp(f->name, im->name)) impl = f;
        }
        if (!impl || impl->is_abstract) diag_error(c->loc, "class '%s' does not implement method '%s' of interface '%s'", c->name, im->name, im->owner->name);
        else if (!same_signature(impl, im)) diag_error(impl->loc, "method '%s' has a different signature than the one declared in interface '%s'", im->name, im->owner->name);
        else if (impl->access == ACC_PRIVATE || impl->access == ACC_PROTECTED) diag_error(impl->loc, "method '%s' implements interface '%s' and must be public", im->name, im->owner->name);
    }
}

/* orders 'prog->classes' with parents before children */
static void order_classes(void) {
    for (int i = 0; i < all_classes.n; i++) setup_class(all_classes.v[i]);
    for (int i = 0; i < all_classes.n; i++) check_contracts(all_classes.v[i]);
    Vec done = {0};
    bool added;
    do {
        added = false;
        for (int i = 0; i < all_classes.n; i++) {
            ClassInfo *c = all_classes.v[i];
            bool present = false;
            for (int j = 0; j < done.n; j++) if (done.v[j] == c) present = true;
            if (present) continue;
            bool parent_ok = !c->parent;
            for (int j = 0; j < done.n && !parent_ok; j++) if (done.v[j] == c->parent) parent_ok = true;
            if (parent_ok) { vec_push(&done, c); added = true; }
        }
    } while (added);
    prog->classes = done;
}

/* ====================== expressions ====================== */
static Type *ex(Node **pn, Type *hint);
static Type *call_expr(Node **pn, Type *hint);
static bool is_generic_recv(Type *t);

static const char *opname(int op) { return token_kind_name((TokKind)op); }

static bool is_lvalue(Node *n) {
    switch (n->kind) {
    case N_IDENT: return n->sym != NULL;
    case N_MEMBER: return n->sym != NULL;
    case N_INDEX: return true;
    case N_UNARY: return n->op == P_STAR;
    default: return false;
    }
}

static Type *implicit_this_type(void) { return cur_cls ? type_class_of(cur_cls) : NULL; }

static Type *ident_expr(Node *n) {
    Symbol *s = lookup_var(n->name);
    if (s) { n->sym = s; s->used = true; return s->type; }
    if (cur_cls) {
        Symbol *st = find_static(cur_cls, n->name);
        if (st) { check_access(st->access, st->owner, n->loc, "static field", n->name); n->sym = st; return st->type; }
        Symbol *f = find_field(cur_cls, n->name);
        if (f && cur_fn && cur_fn->is_static) {
            diag_error(n->loc, "instance field '%s' cannot be used in a static method", n->name);
            return TY_error_t;
        }
        if (f) {
            check_access(f->access, f->owner, n->loc, "field", n->name);
            n->sym = f;
            n->flags |= NF_IMPLICIT_THIS;
            return f->type;
        }
    }
    if (has_function_named(n->name) || (cur_cls && has_method_named(cur_cls, n->name)))
        diag_error(n->loc, "'%s' is a function; use '%s(...)' to call it", n->name, n->name);
    else
        diag_error(n->loc, "variable '%s' is not declared", n->name);
    return TY_error_t;
}

static bool cast_ok(Type *from, Type *to, Loc loc) {
    if (is_err(from) || is_err(to)) return true;
    if (from->kind == TY_VOID || to->kind == TY_VOID) return false;
    bool fnum = from->kind == TY_INT || from->kind == TY_CHAR || from->kind == TY_FLOAT || from->kind == TY_BOOL;
    bool tnum = to->kind == TY_INT || to->kind == TY_CHAR || to->kind == TY_FLOAT || to->kind == TY_BOOL;
    if (fnum && tnum) return true;
    bool fword = from->kind == TY_INT || type_is_reference(from);
    bool tword = to->kind == TY_INT || type_is_reference(to);
    if (from->kind == TY_CLASS && to->kind == TY_CLASS) {
        if (is_subclass(from->cls, to->cls) || is_subclass(to->cls, from->cls)) return true;
        diag_error(loc, "cast between unrelated classes: '%s' and '%s'", from->name, to->name);
        return true;
    }
    if (fword && tword) return true;
    return false;
}

static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 30 || lb > 30) return 99;
    int d[31][31];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int c = a[i - 1] == b[j - 1] ? 0 : 1;
            int m = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + c < m) m = d[i - 1][j - 1] + c;
            d[i][j] = m;
        }
    return d[la][lb];
}

/* looks for a visible variable/field with a similar name (to detect typos) */
static const char *similar_name(const char *name) {
    if (strlen(name) < 3) return NULL;
    const char *best = NULL;
    int bd = 3;
    for (Scope *sc = scope; sc; sc = sc->parent)
        for (int i = 0; i < sc->syms.n; i++) {
            Symbol *y = sc->syms.v[i];
            int d = edit_distance(name, y->name);
            if (d > 0 && d < bd && (d == 1 || strlen(name) >= 6) && y->name[0] != '_') { bd = d; best = y->name; }
        }
    for (int i = 0; i < global_scope->syms.n; i++) {
        Symbol *y = global_scope->syms.v[i];
        int d = edit_distance(name, y->name);
        if (d > 0 && d < bd && (d == 1 || strlen(name) >= 6) && y->name[0] != '_') { bd = d; best = y->name; }
    }
    if (cur_cls)
        for (ClassInfo *c = cur_cls; c; c = c->parent)
            for (int i = 0; i < c->fields.n; i++) {
                Symbol *y = c->fields.v[i];
                int d = edit_distance(name, y->name);
                if (d > 0 && d < bd && (d == 1 || strlen(name) >= 6)) { bd = d; best = y->name; }
            }
    return best;
}

/* ---------- binary operators ---------- */
static Type *binary(Node **pn) {
    Node *n = *pn;
    int op = n->op;
    Type *ta = ex(&n->a, NULL);
    Type *tb = ex(&n->b, NULL);
    if (is_err(ta) || is_err(tb)) return TY_error_t;
    if (ta->kind == TY_VOID || tb->kind == TY_VOID) {
        diag_error(n->loc, "operand of operator %s has no value (void)", opname(op));
        return TY_error_t;
    }
    Loc loc = n->loc;

    if (op == P_ANDAND || op == P_OROR) {
        if (!cond_type_ok(ta) || !cond_type_ok(tb)) {
            diag_error(loc, "operator %s requires boolean/integer operands, got '%s' and '%s'", opname(op), type_str(ta), type_str(tb));
            return TY_error_t;
        }
        return TY_bool_t;
    }

    bool na = type_is_numeric(ta), nb = type_is_numeric(tb);
    bool pa = ta->kind == TY_PTR || ta->kind == TY_PTR_ANY;

    switch (op) {
    case P_PLUS:
        if (ta->kind == TY_STRING || tb->kind == TY_STRING) {                 /* concatenacao */
            Node *a = to_string_node(n->a), *b = to_string_node(n->b);
            *pn = mk_rt(loc, "__concat", a, b, NULL);
            return TY_string_t;
        }
        /* fallthrough */
    case P_MINUS:
        if (pa && type_is_integral(tb) && tb->kind != TY_BOOL) return ta;     /* aritmetica de ponteiros */
        /* fallthrough */
    case P_STAR: case P_SLASH: case P_PERCENT:
        if (na && nb) {
            if (ta->kind == TY_FLOAT || tb->kind == TY_FLOAT) {
                if (op == P_PERCENT) { diag_error(loc, "operator '%%' does not apply to float"); return TY_error_t; }
                n->a = coerce(n->a, TY_float_t, "operand");
                n->b = coerce(n->b, TY_float_t, "operand");
                return TY_float_t;
            }
            return TY_int_t;
        }
        break;
    case P_SHL: case P_SHR:
        if (pa && type_is_integral(tb) && tb->kind != TY_BOOL) return ta;     /* movimento de ponteiros */
        /* fallthrough */
    case P_AMP: case P_PIPE: case P_CARET:
        if (ta->kind == TY_BOOL && tb->kind == TY_BOOL && op != P_SHL && op != P_SHR) return TY_bool_t;
        if (type_is_integral(ta) && type_is_integral(tb) && ta->kind != TY_BOOL && tb->kind != TY_BOOL) return TY_int_t;
        break;
    case P_EQ: case P_NE:
        if (na && nb) {
            if (ta->kind == TY_FLOAT || tb->kind == TY_FLOAT) {
                n->a = coerce(n->a, TY_float_t, "operand");
                n->b = coerce(n->b, TY_float_t, "operand");
            }
            return TY_bool_t;
        }
        if (ta->kind == TY_BOOL && (tb->kind == TY_BOOL)) return TY_bool_t;
        if (ta->kind == TY_STRING && tb->kind == TY_STRING) {                 /* compares contents */
            Node *call = mk_rt(loc, "__streq", n->a, n->b, NULL);
            call->type = TY_bool_t;
            if (op == P_NE) {
                Node *neg = node_new(N_UNARY, loc);
                neg->op = P_BANG; neg->a = call; neg->type = TY_bool_t;
                *pn = neg;
            } else *pn = call;
            return TY_bool_t;
        }
        if (type_is_reference(ta) && type_is_reference(tb)) {
            bool ok = ta->kind == TY_NULL || tb->kind == TY_NULL || type_equal(ta, tb) ||
                      ta->kind == TY_PTR_ANY || tb->kind == TY_PTR_ANY ||
                      (ta->kind == TY_CLASS && tb->kind == TY_CLASS && (is_subclass(ta->cls, tb->cls) || is_subclass(tb->cls, ta->cls)));
            if (ok) return TY_bool_t;
        }
        break;
    case P_LT: case P_LE: case P_GT: case P_GE:
        if (na && nb) {
            if (ta->kind == TY_FLOAT || tb->kind == TY_FLOAT) {
                n->a = coerce(n->a, TY_float_t, "operand");
                n->b = coerce(n->b, TY_float_t, "operand");
            }
            return TY_bool_t;
        }
        if (ta->kind == TY_STRING && tb->kind == TY_STRING) {                 /* lexicographic order */
            Node *call = mk_rt(loc, "__strcmp", n->a, n->b, NULL);
            n->a = call;
            n->b = mk_int(loc, 0);
            return TY_bool_t;
        }
        break;
    default: break;
    }
    diag_error(loc, "operator %s does not apply to types '%s' and '%s'", opname(op), type_str(ta), type_str(tb));
    return TY_error_t;
}

static Type *unary(Node **pn, Type *hint) {
    Node *n = *pn;
    (void)hint;
    Type *t = ex(&n->a, NULL);
    if (is_err(t)) return TY_error_t;
    if (t->kind == TY_VOID) { diag_error(n->loc, "operand has no value (void)"); return TY_error_t; }
    switch (n->op) {
    case P_MINUS:
        if (t->kind == TY_FLOAT) return TY_float_t;
        if (t->kind == TY_INT || t->kind == TY_CHAR) return TY_int_t;
        break;
    case P_BANG:
        if (cond_type_ok(t)) return TY_bool_t;
        break;
    case P_TILDE:
        if (t->kind == TY_INT || t->kind == TY_CHAR) return TY_int_t;
        break;
    case P_STAR:
        if (t->kind == TY_PTR) return t->elem;
        if (t->kind == TY_PTR_ANY) { diag_error(n->loc, "cannot dereference a generic 'ptr'; cast it first: (int*) p"); return TY_error_t; }
        diag_error(n->loc, "the '*' (dereference) operator requires a pointer, got '%s'", type_str(t));
        return TY_error_t;
    case P_AMP:
        if (is_lvalue(n->a)) return type_ptr_to(t);
        diag_error(n->loc, "the '&' operator requires a variable, field or element");
        return TY_error_t;
    }
    diag_error(n->loc, "operator %s does not apply to type '%s'", opname(n->op), type_str(t));
    return TY_error_t;
}

static void check_modifiable(Node *lhs) {
    if (!is_lvalue(lhs)) {
        diag_error(lhs->loc, "the left-hand side cannot be modified (it is not a variable, field or element)");
        return;
    }
    if (lhs->kind == N_IDENT && lhs->sym && lhs->sym->is_const)
        diag_error(lhs->loc, "cannot modify constant '%s' (declared on line %d)", lhs->name, lhs->sym->loc.line);
}

static int base_op(int op) {
    switch (op) {
    case P_PLUSEQ: return P_PLUS;   case P_MINUSEQ: return P_MINUS;
    case P_STAREQ: return P_STAR;   case P_SLASHEQ: return P_SLASH;
    case P_PERCENTEQ: return P_PERCENT; case P_AMPEQ: return P_AMP;
    case P_PIPEEQ: return P_PIPE;   case P_CARETEQ: return P_CARET;
    case P_SHLEQ: return P_SHL;     case P_SHREQ: return P_SHR;
    default: return 0;
    }
}

static Type *assign(Node **pn) {
    Node *n = *pn;
    Node *lhs = n->a;

    if (lhs->kind == N_INDEX && n->op == P_ASSIGN) {          /* list[i] = x  ->  set(i, x);  map[k] = v  ->  put(k, v) */
        Type *bt = ex(&lhs->a, NULL);
        if (!is_err(bt) && is_generic_recv(bt)) {
            Node *callee = node_new(N_MEMBER, n->loc);
            callee->a = lhs->a;
            callee->name = !strcmp(bt->name, "Map") ? "put" : "set";
            Node *k = lhs->b, *v = n->b;
            n->kind = N_CALL; n->a = callee; n->list = (Vec){0};
            vec_push(&n->list, k); vec_push(&n->list, v);
            return call_expr(pn, NULL);
        }
    }

    /* assignment to an unknown name declares the variable (inferred type) */
    if (lhs->kind == N_IDENT && n->op == P_ASSIGN && !lookup_var(lhs->name) &&
        !(cur_cls && find_field(cur_cls, lhs->name))) {
        Type *rt = ex(&n->b, NULL);
        if (rt->kind == TY_VOID || rt->kind == TY_NULL) {
            diag_error(lhs->loc, "cannot infer the type of '%s' from '%s'; declare it with an explicit type", lhs->name, type_str(rt));
            rt = TY_error_t;
        }
        if (prog->strict && !in_prelude(lhs->loc)) {
            diag_error(lhs->loc, "variable '%s' is not declared (strict mode): use 'var %s = ...' or '%s %s = ...'", lhs->name, lhs->name, type_str(rt), lhs->name);
        } else if (!in_prelude(lhs->loc)) {
            const char *sim = similar_name(lhs->name);
            if (sim)
                diag_warning(lhs->loc, "'%s' creates a NEW variable; did you mean '%s'? (if intentional, declare it with 'var %s = ...')", lhs->name, sim, lhs->name);
        }
        Symbol *s = declare_var(lhs->name, rt, false, lhs->loc);
        s->implicit = !in_prelude(lhs->loc);
        if (s->implicit) vec_push(&implicit_syms, s);
        lhs->sym = s;
        lhs->type = rt;
        return rt;
    }

    bool was_used = lhs->kind == N_IDENT && lookup_var(lhs->name) ? lookup_var(lhs->name)->used : false;
    Type *tl = ex(&n->a, NULL);
    if (n->op == P_ASSIGN && lhs->kind == N_IDENT && lhs->sym && lhs->sym->kind != SYM_FIELD) lhs->sym->used = was_used;
    if (is_err(tl)) { ex(&n->b, NULL); return TY_error_t; }
    check_modifiable(n->a);
    Type *tr = ex(&n->b, tl);
    if (is_err(tr)) return tl;

    if (n->op == P_ASSIGN) {
        n->b = coerce(n->b, tl, "assignment");
        return tl;
    }
    /* atribuicao composta */
    int op = base_op(n->op);
    if (tr->kind == TY_VOID) { diag_error(n->b->loc, "the assigned value does not exist (void)"); return tl; }
    if (tl->kind == TY_STRING) {
        if (op != P_PLUS) { diag_error(n->loc, "operator %s does not apply to String", opname(n->op)); return tl; }
        n->b = to_string_node(n->b);
        return tl;
    }
    if (tl->kind == TY_FLOAT) {
        if (op == P_PLUS || op == P_MINUS || op == P_STAR || op == P_SLASH) {
            if (type_is_numeric(tr)) { n->b = coerce(n->b, TY_float_t, "assignment"); return tl; }
        }
    } else if (tl->kind == TY_INT || tl->kind == TY_CHAR) {
        if (type_is_integral(tr) && tr->kind != TY_BOOL) return tl;
        if (tr->kind == TY_FLOAT) {
            diag_error(n->loc, "cannot apply %s with a float to a '%s' variable; use an explicit cast", opname(n->op), type_str(tl));
            return tl;
        }
    } else if (tl->kind == TY_PTR || tl->kind == TY_PTR_ANY) {
        if ((op == P_PLUS || op == P_MINUS || op == P_SHL || op == P_SHR) && type_is_integral(tr)) return tl;
    } else if (tl->kind == TY_BOOL && (op == P_AMP || op == P_PIPE || op == P_CARET) && tr->kind == TY_BOOL) return tl;
    diag_error(n->loc, "operator %s does not apply to types '%s' and '%s'", opname(n->op), type_str(tl), type_str(tr));
    return tl;
}

/* ---------- calls ---------- */
static Node *mk_this(Loc l) {
    Node *t = node_new(N_THIS, l);
    t->type = implicit_this_type();
    return t;
}

static void check_args(Node *call, FuncInfo *f, const char *what) {
    for (int i = 0; i < call->list.n; i++) {
        Symbol *p = f->params.v[((f->owner && !f->is_static) ? 1 : 0) + i];
        Type *at = ex((Node **)&call->list.v[i], p->type);
        if (is_err(at)) continue;
        char *ctx = xsprintf("argument %d of %s '%s'", i + 1, what, f->name);
        call->list.v[i] = coerce(call->list.v[i], p->type, ctx);
    }
}

static bool is_generic_recv(Type *t) { return t->kind == TY_CLASS && t->elem && t->cls && t->cls->is_prelude; }

/* 'ptr' parameters of the List/Map classes stand for the type T: replaced by the element type */
static void check_args_generic(Node *call, FuncInfo *f, Type *elem) {
    for (int i = 0; i < call->list.n; i++) {
        Symbol *p = f->params.v[1 + i];
        Type *want = p->type->kind == TY_PTR_ANY ? elem : p->type;
        Type *at = ex((Node **)&call->list.v[i], want);
        if (is_err(at)) continue;
        call->list.v[i] = coerce(call->list.v[i], want, xsprintf("argument %d of method '%s'", i + 1, f->name));
    }
}

/* loose arguments (when the call already failed) so errors inside them are still reported */
static void analyze_args_only(Node *call) {
    for (int i = 0; i < call->list.n; i++) ex((Node **)&call->list.v[i], NULL);
}

static void arity_error(Loc loc, const char *what, const char *name, int expected, int got) {
    diag_error(loc, "%s '%s' expects %d argument(s) but got %d", what, name, expected, got);
}

static Type *finish_call(Node *n, FuncInfo *f, Node *receiver, bool virt) {
    n->kind = N_CALLFN;
    n->fn = f;
    n->a = receiver;
    if (virt && f->is_virtual) n->flags |= NF_VIRTUAL;
    return f->ret;
}

static Node *arg(Node *call, int i) { return call->list.v[i]; }

static bool is_literal(Node *n) {
    return n->kind == N_INT || n->kind == N_FLOAT || n->kind == N_STR || n->kind == N_CHAR || n->kind == N_BOOL || n->kind == N_NULL;
}
static bool has_effect(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case N_CALLFN: case N_NEW: case N_ASSIGN: case N_INCDEC: case N_BUILTIN: case N_SEQ: case N_CALL: return true;
    default: break;
    }
    if (has_effect(n->a) || has_effect(n->b) || has_effect(n->c)) return true;
    for (int i = 0; i < n->list.n; i++) if (has_effect(n->list.v[i])) return true;
    return false;
}

/*
 * Guarantees "Java/Python-like" evaluation order: all arguments of
 * print/printf are evaluated (left->right) BEFORE anything is printed.
 * When there are side effects, each argument goes to a temporary in the frame.
 */
static void hoist_args(Vec *args, int from, Vec *pre) {
    int nonlit = 0;
    bool eff = false;
    for (int i = from; i < args->n; i++) {
        Node *e = args->v[i];
        if (is_err(e->type)) return;
        if (!is_literal(e)) nonlit++;
        if (has_effect(e)) eff = true;
    }
    if (nonlit < 2 || !eff) return;
    for (int i = from; i < args->n; i++) {
        Node *e = args->v[i];
        if (is_literal(e) || e->type->kind == TY_VOID) continue;
        Symbol *sym = xcalloc(sizeof(Symbol));
        sym->kind = SYM_LOCAL; sym->name = "__t"; sym->type = e->type;
        sym->offset = -8 * (alloc_slot() + 1);
        Node *id = node_new(N_IDENT, e->loc);
        id->name = "__t"; id->sym = sym; id->type = e->type;
        Node *as = node_new(N_ASSIGN, e->loc);
        as->op = P_ASSIGN; as->a = id; as->b = e; as->type = e->type;
        vec_push(pre, as);
        Node *id2 = node_new(N_IDENT, e->loc);
        id2->name = "__t"; id2->sym = sym; id2->type = e->type;
        args->v[i] = id2;
    }
}

/* print / println */
static Type *print_call(Node *n, bool newline) {
    Vec items = {0};
    for (int i = 0; i < n->list.n; i++) ex((Node **)&n->list.v[i], NULL);
    hoist_args(&n->list, 0, &items);
    for (int i = 0; i < n->list.n; i++) {
        Node *e = n->list.v[i];
        Type *t = e->type;
        if (is_err(t)) continue;
        Loc l = e->loc;
        Node *call = NULL;
        switch (t->kind) {
        case TY_INT: call = mk_rt(l, "__print_int", e, NULL, NULL); break;
        case TY_CHAR: call = mk_rt(l, "__print_char", e, NULL, NULL); break;
        case TY_BOOL: call = mk_rt(l, "__print_bool", e, NULL, NULL); break;
        case TY_FLOAT: call = mk_rt(l, "__print_float", e, mk_int(l, -1), NULL); break;
        case TY_STRING: call = mk_rt(l, "__print_str", e, NULL, NULL); break;
        case TY_NULL: call = mk_rt(l, "__print_str", mk_str(l, "null", 4), NULL, NULL); break;
        case TY_CLASS: case TY_ARRAY: case TY_PTR: case TY_PTR_ANY: call = mk_rt(l, "__print_hex", e, mk_int(l, 1), NULL); break;
        default: diag_error(l, "cannot print a value of type '%s'", type_str(t));
        }
        if (call) vec_push(&items, call);
    }
    if (newline) vec_push(&items, mk_rt(n->loc, "__print_nl", NULL, NULL, NULL));
    n->kind = N_SEQ;
    n->list = items;
    return TY_void_t;
}

/* printf with a literal format, converted into typed calls */
static Type *printf_call(Node *n) {
    if (n->list.n == 0) { diag_error(n->loc, "printf requires a format string"); return TY_void_t; }
    Node *fmt = n->list.v[0];
    if (fmt->kind != N_STR) {
        diag_error(fmt->loc, "the printf format must be a string literal");
        analyze_args_only(n);
        return TY_void_t;
    }
    int nargs = n->list.n - 1, used = 0;
    for (int i = 1; i <= nargs; i++) ex((Node **)&n->list.v[1 + i - 1], NULL);
    Vec items = {0};
    hoist_args(&n->list, 1, &items);
    const char *s = fmt->name;
    int len = (int)fmt->ival;
    char *chunk = xcalloc((size_t)len + 1);
    int cl = 0;
    bool ok = true;
    for (int i = 0; i < len; i++) {
        if (s[i] != '%') { chunk[cl++] = s[i]; continue; }
        if (i + 1 < len && s[i + 1] == '%') { chunk[cl++] = '%'; i++; continue; }
        if (cl) { vec_push(&items, mk_rt(fmt->loc, "__print_str", mk_str(fmt->loc, chunk, cl), NULL, NULL)); cl = 0; }
        i++;
        int prec = -1, width = 0, flags = 0;
        while (i < len && (s[i] == '-' || s[i] == '+' || s[i] == ' ' || s[i] == '0' || s[i] == '#')) {
            if (s[i] == '-') flags |= 1;
            if (s[i] == '0') flags |= 2;
            i++;
        }
        while (i < len && s[i] >= '0' && s[i] <= '9') width = width * 10 + (s[i++] - '0');
        if (i < len && s[i] == '.') { i++; prec = 0; while (i < len && s[i] >= '0' && s[i] <= '9') prec = prec * 10 + (s[i++] - '0'); }
        while (i < len && (s[i] == 'l' || s[i] == 'h' || s[i] == 'z')) i++;
        if (i >= len) { diag_error(fmt->loc, "incomplete format specifier at the end of the printf format"); ok = false; break; }
        char conv = s[i];
        if (!strchr("dfscxXpiub", conv)) { diag_error(fmt->loc, "unknown specifier '%%%c' in printf", conv); ok = false; continue; }
        if (used >= nargs) {
            diag_error(fmt->loc, "the printf format expects more arguments than were passed (got %d)", nargs);
            ok = false;
            break;
        }
        Node *e = n->list.v[1 + used++];
        Type *t = e->type;
        if (is_err(t)) continue;
        Loc l = e->loc;
        Node *call = NULL, *sv = NULL;
        if (conv == 'd' || conv == 'i' || conv == 'u') {
            if (t->kind == TY_INT || t->kind == TY_CHAR || t->kind == TY_BOOL) {
                if (width) sv = mk_rt(l, "__itoa", e, NULL, NULL); else call = mk_rt(l, "__print_int", e, NULL, NULL);
            } else diag_error(l, "%%%c expects an integer, got '%s'%s", conv, type_str(t), t->kind == TY_FLOAT ? " (use %f or an (int) cast)" : "");
        } else if (conv == 'f') {
            if (type_is_numeric(t)) {
                Node *ef = coerce(e, TY_float_t, "printf");
                int pr = prec < 0 ? 6 : prec;
                if (width) sv = mk_rt(l, "__ftoa", ef, mk_int(l, pr), NULL); else call = mk_rt(l, "__print_float", ef, mk_int(l, pr), NULL);
            } else diag_error(l, "%%f expects a number, got '%s'", type_str(t));
        } else if (conv == 's') {
            if (t->kind == TY_STRING || t->kind == TY_NULL) {
                Node *se = t->kind == TY_NULL ? mk_str(l, "null", 4) : e;
                if (width) sv = se; else call = mk_rt(l, "__print_str", se, NULL, NULL);
            } else diag_error(l, "%%s expects a String, got '%s'", type_str(t));
        } else if (conv == 'c') {
            if (t->kind == TY_CHAR || t->kind == TY_INT) {
                if (width) sv = mk_rt(l, "__ctoa", e, NULL, NULL); else call = mk_rt(l, "__print_char", e, NULL, NULL);
            } else diag_error(l, "%%c expects a char, got '%s'", type_str(t));
        } else if (conv == 'b') {
            if (t->kind == TY_BOOL) {
                if (width) sv = mk_rt(l, "__btoa", e, NULL, NULL); else call = mk_rt(l, "__print_bool", e, NULL, NULL);
            } else diag_error(l, "%%b expects a bool, got '%s'", type_str(t));
        } else {  /* x X p */
            if (type_is_integral(t) || type_is_reference(t)) {
                int pf = conv == 'p';
                if (width) sv = mk_rt(l, "__xtoa", e, mk_int(l, pf), NULL); else call = mk_rt(l, "__print_hex", e, mk_int(l, pf), NULL);
            } else diag_error(l, "%%%c expects an integer or pointer, got '%s'", conv, type_str(t));
        }
        if (sv) call = mk_rt(l, "__print_pad", sv, mk_int(l, width), mk_int(l, flags));
        if (call) vec_push(&items, call);
    }
    if (cl) vec_push(&items, mk_rt(fmt->loc, "__print_str", mk_str(fmt->loc, chunk, cl), NULL, NULL));
    if (ok && used < nargs)
        diag_error(((Node *)n->list.v[1 + used])->loc, "extra printf arguments: the format uses %d, but %d were passed", used, nargs);
    n->kind = N_SEQ;
    n->list = items;
    return TY_void_t;
}

static Type *builtin_call(Node *n, const char *name, Type *hint, bool *handled) {
    *handled = true;
    int na = n->list.n;
    Loc loc = n->loc;
#define NEED(cnt) do { if (na != (cnt)) { arity_error(loc, "built-in function", name, (cnt), na); analyze_args_only(n); return TY_error_t; } } while (0)

    if (!strcmp(name, "print")) return print_call(n, false);
    if (!strcmp(name, "println")) return print_call(n, true);
    if (!strcmp(name, "printf")) return printf_call(n);

    if (!strcmp(name, "read") || !strcmp(name, "readInt") || !strcmp(name, "readFloat") || !strcmp(name, "readLine")) {
        NEED(0);
        const char *rt = "__read_line";
        Type *t = TY_string_t;
        if (!strcmp(name, "readInt") || (!strcmp(name, "read") && hint && hint->kind == TY_INT)) { rt = "__read_int"; t = TY_int_t; }
        else if (!strcmp(name, "readFloat") || (!strcmp(name, "read") && hint && hint->kind == TY_FLOAT)) { rt = "__read_float"; t = TY_float_t; }
        FuncInfo *f = rt_func(rt);
        n->kind = N_CALLFN; n->fn = f; n->a = NULL;
        return t;
    }
    if (!strcmp(name, "len")) {
        NEED(1);
        Type *t = ex((Node **)&n->list.v[0], NULL);
        if (is_err(t)) return TY_int_t;
        if (t->kind == TY_STRING) { n->kind = N_CALLFN; n->fn = rt_func("__strlen"); n->a = NULL; return TY_int_t; }
        if (t->kind == TY_ARRAY) { n->kind = N_BUILTIN; n->op = BI_ARRLEN; n->a = arg(n, 0); return TY_int_t; }
        diag_error(loc, "len() requires a String or array, got '%s'", type_str(t));
        return TY_int_t;
    }
    if (!strcmp(name, "toString")) {
        NEED(1);
        Type *t = ex((Node **)&n->list.v[0], NULL);
        if (is_err(t)) return TY_string_t;
        Node *conv = to_string_node(arg(n, 0));
        *n = *conv;
        return TY_string_t;
    }
    if (!strcmp(name, "toInt") || !strcmp(name, "toFloat")) {
        NEED(1);
        Type *t = ex((Node **)&n->list.v[0], TY_string_t);
        if (!is_err(t) && t->kind != TY_STRING) diag_error(loc, "%s() requires a String, got '%s'", name, type_str(t));
        n->kind = N_CALLFN; n->a = NULL;
        n->fn = rt_func(!strcmp(name, "toInt") ? "__atoi" : "__atof");
        return !strcmp(name, "toInt") ? TY_int_t : TY_float_t;
    }
    if (!strcmp(name, "abs") || !strcmp(name, "min") || !strcmp(name, "max") || !strcmp(name, "pow")) {
        bool is_pow = !strcmp(name, "pow"), is_abs = !strcmp(name, "abs");
        NEED(is_abs ? 1 : 2);
        bool anyfloat = false, bad = false;
        for (int i = 0; i < na; i++) {
            Type *t = ex((Node **)&n->list.v[i], NULL);
            if (is_err(t)) { bad = true; continue; }
            if (t->kind == TY_FLOAT) anyfloat = true;
            else if (!(t->kind == TY_INT || t->kind == TY_CHAR)) { diag_error(arg(n, i)->loc, "%s() requires numbers, got '%s'", name, type_str(t)); bad = true; }
        }
        if (bad) return TY_error_t;
        const char *rt;
        if (is_pow) {
            if (arg(n, 1)->type->kind == TY_FLOAT) { diag_error(arg(n, 1)->loc, "pow(base, exponent): the exponent must be an integer"); return TY_error_t; }
            n->list.v[0] = coerce(arg(n, 0), TY_float_t, "pow");
            rt = "__powi";
        } else {
            if (anyfloat) for (int i = 0; i < na; i++) n->list.v[i] = coerce(arg(n, i), TY_float_t, name);
            rt = is_abs ? (anyfloat ? "__absf" : "__absi") : !strcmp(name, "min") ? (anyfloat ? "__minf" : "__mini") : (anyfloat ? "__maxf" : "__maxi");
        }
        n->kind = N_CALLFN; n->a = NULL; n->fn = rt_func(rt);
        return (is_pow || anyfloat) ? TY_float_t : TY_int_t;
    }
    if (!strcmp(name, "sqrt")) {
        NEED(1);
        Type *t = ex((Node **)&n->list.v[0], TY_float_t);
        if (!is_err(t)) n->list.v[0] = coerce(arg(n, 0), TY_float_t, "sqrt argument");
        n->kind = N_BUILTIN; n->op = BI_SQRT; n->a = arg(n, 0);
        return TY_float_t;
    }
    if (!strcmp(name, "exit")) {
        NEED(1);
        Type *t = ex((Node **)&n->list.v[0], TY_int_t);
        if (!is_err(t)) n->list.v[0] = coerce(arg(n, 0), TY_int_t, "exit argument");
        n->kind = N_CALLFN; n->a = NULL; n->fn = rt_func("__exit");
        return TY_void_t;
    }
    if (!strcmp(name, "setMen") || !strcmp(name, "freeMEM")) {
        if (!prog->no_gc) {
            diag_error(loc, "'%s' can only be used in manual-memory mode: add the #NOT_GARBAGE directive at the top of the file", name);
            diag_note(loc, "by default the language uses the garbage collector, which frees memory automatically");
            analyze_args_only(n);
            return !strcmp(name, "setMen") ? TY_ptrany_t : TY_void_t;
        }
        NEED(1);
        bool alloc = !strcmp(name, "setMen");
        Type *t = ex((Node **)&n->list.v[0], alloc ? TY_int_t : NULL);
        if (!is_err(t)) {
            if (alloc && !(t->kind == TY_INT || t->kind == TY_CHAR)) diag_error(loc, "setMen() requires the size in bytes (int), got '%s'", type_str(t));
            if (!alloc && !type_is_reference(t)) diag_error(loc, "freeMEM() requires a pointer/object, got '%s'", type_str(t));
        }
        n->kind = N_CALLFN; n->a = NULL; n->fn = rt_func(alloc ? "__alloc" : "__free");
        return alloc ? TY_ptrany_t : TY_void_t;
    }
    if (!strcmp(name, "push_back")) {
        diag_error(loc, "'push_back' is a List method, not a global function: use list.push_back(x) with a List<T>");
        analyze_args_only(n);
        return TY_error_t;
    }
    if (name[0] == '_' && name[1] == '_' && in_prelude(loc)) {                  /* intrinsicos do runtime */
        static const struct { const char *n; int bi; int args; Type **ret; } INTR[] = {
            {"__winapi", BI_WINAPI, -1, &TY_int_t}, {"__peek8", BI_PEEK8, 1, &TY_int_t},
            {"__poke8", BI_POKE8, 2, &TY_void_t}, {"__peek64", BI_PEEK64, 1, &TY_int_t},
            {"__poke64", BI_POKE64, 2, &TY_void_t}, {"__sp", BI_SP, 0, &TY_int_t},
            {"__gbegin", BI_GBEGIN, 0, &TY_int_t}, {"__gend", BI_GEND, 0, &TY_int_t},
            {"__memcpy", BI_MEMCPY, 3, &TY_void_t}, {NULL, 0, 0, NULL}};
        for (int i = 0; INTR[i].n; i++) {
            if (strcmp(INTR[i].n, name)) continue;
            if (INTR[i].args >= 0 && na != INTR[i].args) { arity_error(loc, "intrinsic", name, INTR[i].args, na); return TY_error_t; }
            if (INTR[i].args < 0 && (na < 1 || na > 6 || arg(n, 0)->kind != N_INT)) {
                diag_error(loc, "__winapi requires the API index (literal) and up to 5 arguments");
                return TY_error_t;
            }
            for (int k = (INTR[i].args < 0 ? 1 : 0); k < na; k++) {
                ex((Node **)&n->list.v[k], NULL);
                Type *t = arg(n, k)->type;
                if (!is_err(t) && t->kind == TY_FLOAT) diag_error(arg(n, k)->loc, "intrinsic '%s' does not accept float", name);
            }
            n->kind = N_BUILTIN;
            n->op = INTR[i].bi;
            return *INTR[i].ret;
        }
    }
    *handled = false;
    return NULL;
#undef NEED
}

/* built-in String methods */
static Type *string_method(Node *n, Node *recv, const char *name) {
    int na = n->list.n;
    Loc loc = n->loc;
    if (!strcmp(name, "length") && na == 0) { n->kind = N_CALLFN; n->fn = rt_func("__strlen"); n->a = NULL; n->list = (Vec){0}; vec_push(&n->list, recv); return TY_int_t; }
    if (!strcmp(name, "charAt") && na == 1) {
        Type *t = ex((Node **)&n->list.v[0], TY_int_t);
        if (!is_err(t) && !type_is_integral(t)) diag_error(loc, "charAt() requires an integer index");
        n->kind = N_INDEX; n->a = recv; n->b = arg(n, 0); n->list = (Vec){0};
        return TY_char_t;
    }
    if (!strcmp(name, "equals") && na == 1) {
        Type *t = ex((Node **)&n->list.v[0], TY_string_t);
        if (!is_err(t) && t->kind != TY_STRING && t->kind != TY_NULL) diag_error(loc, "equals() requires a String, got '%s'", type_str(t));
        n->kind = N_CALLFN; n->fn = rt_func("__streq"); n->a = NULL;
        Node *o = arg(n, 0);
        n->list = (Vec){0};
        vec_push(&n->list, recv); vec_push(&n->list, o);
        return TY_bool_t;
    }
    if (!strcmp(name, "indexOf") && na == 1) {
        Type *t = ex((Node **)&n->list.v[0], TY_string_t);
        if (!is_err(t) && t->kind != TY_STRING) diag_error(loc, "indexOf() requires a String, got '%s'", type_str(t));
        n->kind = N_CALLFN; n->fn = rt_func("__indexof"); n->a = NULL;
        Node *o = arg(n, 0);
        n->list = (Vec){0};
        vec_push(&n->list, recv); vec_push(&n->list, o);
        return TY_int_t;
    }
    if (!strcmp(name, "substring") && na == 2) {
        for (int i = 0; i < 2; i++) {
            Type *t = ex((Node **)&n->list.v[i], TY_int_t);
            if (!is_err(t) && !type_is_integral(t)) diag_error(loc, "substring() requires integer indices");
        }
        n->kind = N_CALLFN; n->fn = rt_func("__substr"); n->a = NULL;
        Node *x = arg(n, 0), *y = arg(n, 1);
        n->list = (Vec){0};
        vec_push(&n->list, recv); vec_push(&n->list, x); vec_push(&n->list, y);
        return TY_string_t;
    }
    static const struct { const char *name; int nargs; const char *rt; char ret; const char *args; } SM[] = {
        {"trim", 0, "__trim", 'S', ""}, {"toUpperCase", 0, "__upper", 'S', ""}, {"toLowerCase", 0, "__lower", 'S', ""},
        {"isEmpty", 0, "__isempty", 'B', ""}, {"contains", 1, "__contains", 'B', "S"},
        {"startsWith", 1, "__startswith", 'B', "S"}, {"endsWith", 1, "__endswith", 'B', "S"},
        {"replace", 2, "__replace", 'S', "SS"}, {"split", 1, "__split", 'A', "S"}, {"repeat", 1, "__repeat", 'S', "I"},
        {NULL, 0, NULL, 0, NULL}};
    for (int k = 0; SM[k].name; k++) {
        if (strcmp(SM[k].name, name) || SM[k].nargs != na) continue;
        for (int i = 0; i < na; i++) {
            bool want_str = SM[k].args[i] == 'S';
            Type *t = ex((Node **)&n->list.v[i], want_str ? TY_string_t : TY_int_t);
            if (is_err(t)) continue;
            if (want_str && t->kind != TY_STRING) diag_error(arg(n, i)->loc, "%s() requires a String in argument %d, got '%s'", name, i + 1, type_str(t));
            if (!want_str && !(t->kind == TY_INT || t->kind == TY_CHAR)) diag_error(arg(n, i)->loc, "%s() requires an integer in argument %d, got '%s'", name, i + 1, type_str(t));
        }
        Vec nl = {0};
        vec_push(&nl, recv);
        for (int i = 0; i < na; i++) vec_push(&nl, n->list.v[i]);
        n->list = nl;
        n->kind = N_CALLFN; n->fn = rt_func(SM[k].rt); n->a = NULL;
        return SM[k].ret == 'S' ? TY_string_t : SM[k].ret == 'B' ? TY_bool_t : type_array_of(TY_string_t);
    }
    diag_error(loc, "String has no method '%s' with %d argument(s) (available: length, charAt, equals, indexOf, substring, trim, toUpperCase, toLowerCase, isEmpty, contains, startsWith, endsWith, replace, split, repeat)", name, na);
    analyze_args_only(n);
    return TY_error_t;
}

static Type *call_expr(Node **pn, Type *hint) {
    Node *n = *pn;
    Node *callee = n->a;
    int nargs = n->list.n;
    Loc loc = n->loc;

    /* super(...) */
    if (callee->kind == N_SUPER) {
        if (!cur_fn || !cur_fn->is_ctor || !cur_cls) { diag_error(loc, "super(...) can only be used inside a constructor"); analyze_args_only(n); return TY_void_t; }
        if (!cur_cls->parent) { diag_error(loc, "class '%s' has no superclass", cur_cls->name); analyze_args_only(n); return TY_void_t; }
        if (n != allowed_super) { diag_error(loc, "super(...) must be the first statement of the constructor"); analyze_args_only(n); return TY_void_t; }
        FuncInfo *ctor = NULL;
        for (int i = 0; i < cur_cls->parent->ctors.n; i++) {
            FuncInfo *c = cur_cls->parent->ctors.v[i];
            if (c->nargs == nargs) ctor = c;
        }
        if (!ctor) {
            diag_error(loc, "superclass '%s' has no constructor with %d argument(s)", cur_cls->parent->name, nargs);
            analyze_args_only(n);
            return TY_void_t;
        }
        check_access(ctor->access, ctor->owner, loc, "constructor", ctor->name);
        check_args(n, ctor, "constructor");
        finish_call(n, ctor, mk_this(loc), false);
        n->flags |= NF_SUPERCALL;
        return TY_void_t;
    }

    if (callee->kind == N_IDENT) {
        const char *name = callee->name;
        /* 1) method of the current class (implicit this) */
        if (cur_cls) {
            FuncInfo *m = find_method(cur_cls, name, nargs);
            if (m) {
                check_access(m->access, m->owner, loc, "method", name);
                check_args(n, m, "method");
                if (m->is_static) { finish_call(n, m, NULL, false); return m->ret; }
                if (cur_fn && cur_fn->is_static) { diag_error(loc, "instance method '%s' cannot be called from a static method without an object", name); finish_call(n, m, NULL, false); return m->ret; }
                finish_call(n, m, mk_this(loc), true);
                return m->ret;
            }
        }
        /* 2) global function */
        FuncInfo *f = find_function(name, nargs);
        if (f) {
            check_args(n, f, "function");
            finish_call(n, f, NULL, false);
            return f->ret;
        }
        /* 3) built-ins (when there is no user function with the same name) */
        if (!has_function_named(name) && !(cur_cls && has_method_named(cur_cls, name))) {
            bool handled;
            Type *t = builtin_call(n, name, hint, &handled);
            if (handled) return t;
        }
        /* errors */
        if (cur_cls && has_method_named(cur_cls, name)) {
            for (FuncInfo *m = NULL; ; ) {
                for (ClassInfo *c = cur_cls; c && !m; c = c->parent)
                    for (int i = 0; i < c->methods.n; i++) if (!strcmp(((FuncInfo *)c->methods.v[i])->name, name)) { m = c->methods.v[i]; break; }
                if (m) arity_error(loc, "method", name, m->nargs, nargs);
                break;
            }
        } else if (has_function_named(name)) {
            for (int i = 0; i < all_funcs.n; i++) {
                FuncInfo *g = all_funcs.v[i];
                if (!strcmp(g->name, name)) { arity_error(loc, "function", name, g->nargs, nargs); break; }
            }
        } else {
            diag_error(loc, "function '%s' is not declared", name);
        }
        analyze_args_only(n);
        return TY_error_t;
    }

    if (callee->kind == N_MEMBER) {
        const char *name = callee->name;
        /* super.method(...) */
        if (callee->a->kind == N_SUPER) {
            if (!cur_cls || !cur_cls->parent) { diag_error(loc, "'super' used in a class without a superclass"); analyze_args_only(n); return TY_error_t; }
            FuncInfo *m = find_method(cur_cls->parent, name, nargs);
            if (!m) { diag_error(loc, "superclass '%s' has no method '%s' with %d argument(s)", cur_cls->parent->name, name, nargs); analyze_args_only(n); return TY_error_t; }
            check_access(m->access, m->owner, loc, "method", name);
            check_args(n, m, "method");
            finish_call(n, m, mk_this(loc), false);
            n->flags |= NF_SUPERCALL;
            return m->ret;
        }
        if (callee->a->kind == N_IDENT && !lookup_var(callee->a->name) && !(cur_cls && find_field(cur_cls, callee->a->name)) && find_class(callee->a->name)) {
            ClassInfo *sc = find_class(callee->a->name);                 /* Class.staticMethod(...) */
            FuncInfo *m = find_method(sc, name, nargs);
            if (!m || !m->is_static) {
                if (m) diag_error(loc, "'%s' is an instance method of '%s'; call it on an object", name, sc->name);
                else diag_error(loc, "class '%s' has no static method '%s' with %d argument(s)", sc->name, name, nargs);
                analyze_args_only(n);
                return TY_error_t;
            }
            check_access(m->access, m->owner, loc, "method", name);
            check_args(n, m, "method");
            finish_call(n, m, NULL, false);
            return m->ret;
        }
        Type *rt = ex(&callee->a, NULL);
        if (is_err(rt)) { analyze_args_only(n); return TY_error_t; }
        Node *recv = callee->a;
        if (rt->kind == TY_STRING) return string_method(n, recv, name);
        if (rt->kind == TY_ARRAY && !strcmp(name, "length") && nargs == 0) {
            n->kind = N_BUILTIN; n->op = BI_ARRLEN; n->a = recv; n->list = (Vec){0};
            return TY_int_t;
        }
        if (rt->kind != TY_CLASS) {
            diag_error(loc, "type '%s' has no methods", type_str(rt));
            analyze_args_only(n);
            return TY_error_t;
        }
        FuncInfo *m = find_method(rt->cls, name, nargs);
        if (!m) {
            if (has_method_named(rt->cls, name)) {
                FuncInfo *any = NULL;
                for (ClassInfo *c = rt->cls; c && !any; c = c->parent)
                    for (int i = 0; i < c->methods.n; i++) if (!strcmp(((FuncInfo *)c->methods.v[i])->name, name)) { any = c->methods.v[i]; break; }
                arity_error(loc, "method", name, any->nargs, nargs);
            } else if (find_field(rt->cls, name)) diag_error(loc, "'%s' is a field of '%s', not a method", name, rt->cls->name);
            else diag_error(loc, "class '%s' has no method '%s'", rt->cls->name, name);
            analyze_args_only(n);
            return TY_error_t;
        }
        check_access(m->access, m->owner, loc, "method", name);
        if (is_generic_recv(rt)) {
            check_args_generic(n, m, rt->elem);
            finish_call(n, m, recv, true);
            return m->ret->kind == TY_PTR_ANY ? rt->elem : m->ret;
        }
        check_args(n, m, "method");
        if (m->is_static) { diag_error(loc, "'%s' is static: call it as %s.%s(...)", name, m->owner->name, name); finish_call(n, m, NULL, false); return m->ret; }
        finish_call(n, m, recv, true);
        return m->ret;
    }

    diag_error(loc, "this expression cannot be called as a function");
    analyze_args_only(n);
    return TY_error_t;
}

/* ---------- other expressions ---------- */
static Type *member_expr(Node **pn) {
    Node *n = *pn;
    if (n->a->kind == N_IDENT && !lookup_var(n->a->name) && !(cur_cls && find_field(cur_cls, n->a->name))) {
        Node *en = find_enum(n->a->name);
        if (en) {                                                /* Cor.AZUL -> constante inteira */
            for (int i = 0; i < en->list.n; i++) {
                Node *c = en->list.v[i];
                if (!strcmp(c->name, n->name)) { n->kind = N_INT; n->ival = c->ival; return TY_int_t; }
            }
            diag_error(n->loc, "enum '%s' has no constant '%s'", en->name, n->name);
            return TY_error_t;
        }
    }
    if (n->a->kind == N_IDENT && !lookup_var(n->a->name) && !(cur_cls && find_field(cur_cls, n->a->name)) && find_class(n->a->name)) {
        ClassInfo *sc = find_class(n->a->name);                           /* Class.staticField */
        Symbol *st = find_static(sc, n->name);
        if (!st) { diag_error(n->loc, "class '%s' has no static field '%s'", sc->name, n->name); return TY_error_t; }
        check_access(st->access, st->owner, n->loc, "static field", n->name);
        n->kind = N_IDENT;
        n->sym = st;
        return st->type;
    }
    Type *t = ex(&n->a, NULL);
    if (is_err(t)) return TY_error_t;
    if (is_generic_recv(t) && !strcmp(t->name, "List") && !strcmp(n->name, "length")) {   /* list.length == list.size() */
        Node *callee = node_new(N_MEMBER, n->loc);
        callee->a = n->a; callee->name = "size";
        n->kind = N_CALL; n->a = callee; n->list = (Vec){0};
        return call_expr(pn, NULL);
    }
    if (t->kind == TY_ARRAY && !strcmp(n->name, "length")) { n->flags |= NF_ARRLEN; return TY_int_t; }
    if (t->kind == TY_STRING && !strcmp(n->name, "length")) {
        diag_error(n->loc, "on String, 'length' is a method: use text.length() or len(text)");
        return TY_error_t;
    }
    if (t->kind != TY_CLASS) {
        diag_error(n->loc, "type '%s' has no members", type_str(t));
        return TY_error_t;
    }
    Symbol *f = find_field(t->cls, n->name);
    if (!f) {
        if (has_method_named(t->cls, n->name)) diag_error(n->loc, "'%s' is a method of '%s'; use '%s(...)'", n->name, t->cls->name, n->name);
        else diag_error(n->loc, "class '%s' has no field '%s'", t->cls->name, n->name);
        return TY_error_t;
    }
    check_access(f->access, f->owner, n->loc, "field", n->name);
    n->sym = f;
    return f->type;
}

static Type *index_expr(Node **pn) {
    Node *n = *pn;
    Type *t = ex(&n->a, NULL);
    if (is_generic_recv(t)) {                                  /* list[i] and map["key"] read through get() */
        Node *callee = node_new(N_MEMBER, n->loc);
        callee->a = n->a; callee->name = "get";
        Node *idx = n->b;
        n->kind = N_CALL; n->a = callee; n->list = (Vec){0}; vec_push(&n->list, idx);
        return call_expr(pn, NULL);
    }
    Type *ti = ex(&n->b, TY_int_t);
    if (is_err(t) || is_err(ti)) return TY_error_t;
    if (!(ti->kind == TY_INT || ti->kind == TY_CHAR)) {
        diag_error(n->b->loc, "the index must be an integer, got '%s'", type_str(ti));
        return TY_error_t;
    }
    if (t->kind == TY_ARRAY) return t->elem;
    if (t->kind == TY_STRING) return TY_char_t;
    if (t->kind == TY_PTR) return t->elem;
    diag_error(n->loc, "type '%s' cannot be indexed", type_str(t));
    return TY_error_t;
}

static Type *new_expr(Node *n) {
    n->type = resolve_type(n->type);
    if (is_err(n->type)) { analyze_args_only(n); return TY_error_t; }
    ClassInfo *c = n->type->cls;
    int nargs = n->list.n;
    if (c->is_interface || c->is_abstract) {
        diag_error(n->loc, "cannot create an instance of %s '%s'", c->is_interface ? "interface" : "abstract class", c->name);
        analyze_args_only(n);
        return n->type;
    }
    FuncInfo *ctor = NULL;
    for (int i = 0; i < c->ctors.n; i++) if (((FuncInfo *)c->ctors.v[i])->nargs == nargs) ctor = c->ctors.v[i];
    if (!ctor) {
        FuncInfo *first = c->ctors.v[0];
        diag_error(n->loc, "class '%s' has no constructor with %d argument(s)%s", c->name, nargs,
                   c->ctors.n == 1 ? xsprintf(" (the constructor expects %d)", first->nargs) : "");
        analyze_args_only(n);
        return n->type;
    }
    check_access(ctor->access, ctor->owner, n->loc, "constructor", c->name);
    check_args(n, ctor, "constructor");
    n->fn = ctor;
    n->cls = c;
    return n->type;
}

static Type *arrlit_expr(Node *n, Type *hint) {
    Type *at = n->type ? resolve_type(n->type) : NULL;
    if (!at && hint && hint->kind == TY_ARRAY) at = hint;
    Type *elem = at && !is_err(at) ? at->elem : NULL;
    if (at && is_err(at)) { for (int i = 0; i < n->list.n; i++) ex((Node **)&n->list.v[i], NULL); return TY_error_t; }
    for (int i = 0; i < n->list.n; i++) {
        Type *t = ex((Node **)&n->list.v[i], elem);
        if (is_err(t)) continue;
        if (!elem) {
            if (t->kind == TY_VOID || t->kind == TY_NULL) { diag_error(n->loc, "cannot infer the array element type; declare it: int[] a = {...}"); return TY_error_t; }
            elem = t;
        } else n->list.v[i] = coerce(n->list.v[i], elem, xsprintf("array element %d", i + 1));
    }
    if (!elem) { diag_error(n->loc, "an empty array literal needs a type: int[] a = {};  or  new int[]{}"); return TY_error_t; }
    n->type = type_array_of(elem);
    return n->type;
}

static bool const_label(Node *n, int64_t *v) {
    if (n->kind == N_INT || n->kind == N_CHAR || n->kind == N_BOOL) { *v = n->ival; return true; }
    if (n->kind == N_UNARY && n->op == P_MINUS && const_label(n->a, v)) { *v = -*v; return true; }
    return false;
}

static Type *ex(Node **pn, Type *hint) {
    Node *n = *pn;
    Type *t = TY_error_t;
    switch (n->kind) {
    case N_INT: t = TY_int_t; break;
    case N_FLOAT: t = TY_float_t; break;
    case N_CHAR: t = TY_char_t; break;
    case N_BOOL: t = TY_bool_t; break;
    case N_STR: t = TY_string_t; break;
    case N_NULL: t = TY_null_t; break;
    case N_THIS:
        if (!cur_cls || (cur_fn && cur_fn->is_static)) diag_error(n->loc, "'this' can only be used inside instance methods");
        else t = implicit_this_type();
        break;
    case N_SUPER:
        diag_error(n->loc, "'super' can only be used as super(...) or super.method(...)");
        break;
    case N_IDENT: t = ident_expr(n); break;
    case N_UNARY: t = unary(pn, hint); break;
    case N_BINARY: t = binary(pn); break;
    case N_ASSIGN: t = assign(pn); break;
    case N_INCDEC: {
        Type *tt = ex(&n->a, NULL);
        if (is_err(tt)) break;
        check_modifiable(n->a);
        if (tt->kind == TY_INT || tt->kind == TY_CHAR || tt->kind == TY_FLOAT || tt->kind == TY_PTR || tt->kind == TY_PTR_ANY) t = tt;
        else diag_error(n->loc, "operator %s does not apply to type '%s'", opname(n->op), type_str(tt));
        break;
    }
    case N_CALL: t = call_expr(pn, hint); n = *pn; break;
    case N_MEMBER: t = member_expr(pn); break;
    case N_INDEX: t = index_expr(pn); break;
    case N_NEW: t = new_expr(n); break;
    case N_NEWARRAY: {
        Type *at = resolve_type(n->type);
        Type *ts = ex(&n->a, TY_int_t);
        if (!is_err(ts) && !(ts->kind == TY_INT || ts->kind == TY_CHAR)) diag_error(n->a->loc, "the array size must be an integer");
        if (n->b) {
            Type *t2 = ex(&n->b, TY_int_t);
            if (!is_err(t2) && !(t2->kind == TY_INT || t2->kind == TY_CHAR)) diag_error(n->b->loc, "the array size must be an integer");
        }
        if (!is_err(at) && at->elem->kind == TY_VOID) { diag_error(n->loc, "arrays of void do not exist"); break; }
        n->type = at;
        t = at;
        break;
    }
    case N_CAST: {
        Type *to = resolve_type(n->type);
        Type *from = ex(&n->a, NULL);
        if (is_err(to)) break;
        if (!is_err(from) && !cast_ok(from, to, n->loc))
            diag_error(n->loc, "invalid cast from '%s' to '%s'", type_str(from), type_str(to));
        n->type = to;
        t = to;
        break;
    }
    case N_COND: {
        Type *tc = ex(&n->a, NULL);
        if (!is_err(tc) && !cond_type_ok(tc)) diag_error(n->a->loc, "the '?:' condition must be boolean/integer, got '%s'", type_str(tc));
        Type *t1 = ex(&n->b, hint), *t2 = ex(&n->c, hint);
        if (is_err(t1) || is_err(t2)) break;
        if (type_equal(t1, t2)) t = t1;
        else if (type_is_numeric(t1) && type_is_numeric(t2)) {
            if (t1->kind == TY_FLOAT || t2->kind == TY_FLOAT) {
                n->b = coerce(n->b, TY_float_t, "'?:'"); n->c = coerce(n->c, TY_float_t, "'?:'"); t = TY_float_t;
            } else t = TY_int_t;
        } else if (t1->kind == TY_NULL && type_is_reference(t2)) t = t2;
        else if (t2->kind == TY_NULL && type_is_reference(t1)) t = t1;
        else diag_error(n->loc, "the two branches of '?:' have different types: '%s' and '%s'", type_str(t1), type_str(t2));
        break;
    }
    case N_SIZEOF: {
        Type *st = resolve_type(n->type);
        if (is_err(st)) break;
        if (st->kind == TY_VOID) { diag_error(n->loc, "sizeof(void) does not exist"); break; }
        n->kind = N_INT;
        n->ival = st->kind == TY_CHAR ? 1 : 8;
        t = TY_int_t;
        break;
    }
    case N_INSTANCEOF: {
        Type *tv = ex(&n->a, NULL);
        Type *tt = resolve_type(n->type);
        if (is_err(tv) || is_err(tt)) { t = TY_bool_t; break; }
        if (tv->kind != TY_CLASS || tt->kind != TY_CLASS) {
            diag_error(n->loc, "instanceof only applies to objects and classes (got '%s' instanceof '%s')", type_str(tv), type_str(tt));
        } else if (tt->cls->is_interface) {
            diag_error(n->loc, "instanceof with an interface is not supported yet; use a cast or a class");
        } else if (!is_subclass(tv->cls, tt->cls) && !is_subclass(tt->cls, tv->cls)) {
            diag_warning(n->loc, "'%s' can never be an instance of '%s' (unrelated classes): the result is always false", tv->name, tt->name);
        }
        n->cls = tt->cls;
        t = TY_bool_t;
        break;
    }
    case N_ARRLIT: t = arrlit_expr(n, hint); break;
    case N_CALLFN: case N_BUILTIN: case N_SEQ: t = n->type; break;   /* already analyzed */
    default:
        diag_error(n->loc, "internal error: unexpected expression node (%d)", n->kind);
    }
    n = *pn;
    n->type = t;
    return t;
}

/* ====================== statements ====================== */
static void stmt(Node **pn);

static void cond_expr(Node **pn, const char *what) {
    Type *t = ex(pn, NULL);
    if (is_err(t)) return;
    if (!cond_type_ok(t)) diag_error((*pn)->loc, "the '%s' condition must be boolean/integer, got '%s'", what, type_str(t));
}

/* body of if/while/for: opens its own scope even without braces */
static void scoped_stmt(Node **pn) {
    push_scope();
    int saved = next_slot;
    stmt(pn);
    next_slot = saved;
    pop_scope();
}

static void var_decl(Node *n) {
    Type *dt = n->type ? resolve_type(n->type) : NULL;
    Type *t = dt;
    if (dt && dt->kind == TY_VOID) { diag_error(n->loc, "variable '%s' cannot be of type void", n->name); t = dt = TY_error_t; }
    if (n->a) {
        Type *it = ex(&n->a, dt);
        if (!dt) {
            if (it->kind == TY_VOID || it->kind == TY_NULL) {
                diag_error(n->loc, "cannot infer the type of '%s' from '%s'; declare it with an explicit type", n->name, type_str(it));
                it = TY_error_t;
            }
            t = it;
        } else {
            n->a = coerce(n->a, dt, xsprintf("initialization of '%s'", n->name));
        }
    }
    n->type = t;
    n->sym = declare_var(n->name, t, (n->flags & NF_CONST) != 0, n->loc);
    if (n->sym->kind == SYM_GLOBAL) n->flags |= NF_GLOBALINIT;
}

static Node *first_ident(Node *n) {
    if (!n) return NULL;
    if (n->kind == N_IDENT) return n;
    Node *r = first_ident(n->a);
    return r ? r : first_ident(n->b);
}

static bool always_returns(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case N_RETURN: case N_THROW: return true;
    case N_BLOCK:
        for (int i = 0; i < n->list.n; i++) if (always_returns(n->list.v[i])) return true;
        return false;
    case N_IF: return n->c && always_returns(n->b) && always_returns(n->c);
    case N_WHILE: return n->a->kind == N_BOOL && n->a->ival;
    case N_FOR: return n->b == NULL;
    case N_TRY: {
        if (n->b && always_returns(n->b)) return true;
        if (!always_returns(n->a)) return false;
        for (int i = 0; i < n->list.n; i++) if (!always_returns(((Node *)n->list.v[i])->a)) return false;
        return true;
    }
    default: return false;
    }
}

static void stmt(Node **pn) {
    Node *n = *pn;
    switch (n->kind) {
    case N_EMPTY: break;
    case N_BLOCK: {
        bool ns = (n->flags & NF_NOSCOPE) != 0;
        int saved = next_slot;
        if (!ns) push_scope();
        for (int i = 0; i < n->list.n; i++) stmt((Node **)&n->list.v[i]);
        if (!ns) { pop_scope(); next_slot = saved; }
        break;
    }
    case N_VAR:
        if (n->sym) {                                   /* static field: symbol already created */
            ClassInfo *sv = cur_cls;
            cur_cls = n->cls;
            ex(&n->a, n->sym->type);
            n->a = coerce(n->a, n->sym->type, xsprintf("initialization of '%s'", n->sym->name));
            cur_cls = sv;
        } else var_decl(n);
        break;
    case N_EXPRSTMT: {
        int saved_slot = next_slot;
        ex(&n->a, NULL);
        if (n->a->kind == N_SEQ) next_slot = saved_slot;      /* print temporaries do not need to survive */
        NodeKind k = n->a->kind;
        if (k == N_IDENT || k == N_INT || k == N_FLOAT || k == N_STR || k == N_BINARY || k == N_CHAR || k == N_BOOL)
            diag_warning(n->a->loc, "expression has no effect (the result is discarded)");
        break;
    }
    case N_IF:
        cond_expr(&n->a, "if");
        scoped_stmt(&n->b);
        if (n->c) scoped_stmt(&n->c);
        break;
    case N_WHILE:
        cond_expr(&n->a, "while");
        loop_depth++; scoped_stmt(&n->b); loop_depth--;
        break;
    case N_DO:
        loop_depth++; scoped_stmt(&n->a); loop_depth--;
        cond_expr(&n->b, "do-while");
        break;
    case N_FOR: {
        push_scope();
        int saved = next_slot;
        if (n->flags & NF_FOR_IMPLICIT) {                 /* for(int _; cond; passo) */
            Node *id = first_ident(n->b);
            if (!id) diag_error(n->loc, "'for(int _; ...)' needs a condition that uses the control variable");
            else {
                Node *v = node_new(N_VAR, n->loc);
                v->name = id->name;
                v->type = n->type;
                n->a = v;
            }
        }
        if (n->flags & NF_FOR_PREDECL) {                  /* for(_; cond; passo) */
            Node *id = first_ident(n->b);
            if (id && !lookup_var(id->name) && !(cur_cls && find_field(cur_cls, id->name))) {
                diag_error(id->loc, "variable '%s' is not declared", id->name);
                diag_note(id->loc, "with 'for(_; ...)' the variable must exist before the loop (e.g.: int %s = 0;); use 'for(int _; ...)' to declare it in the for itself", id->name);
                n->b = NULL;
            }
        }
        if (n->a) stmt(&n->a);
        if (n->b) cond_expr(&n->b, "for");
        if (n->c) ex(&n->c, NULL);
        loop_depth++; scoped_stmt(&n->d); loop_depth--;
        next_slot = saved;
        pop_scope();
        break;
    }
    case N_RETURN: {
        if (in_global_init) { diag_error(n->loc, "'return' outside a function"); if (n->a) ex(&n->a, NULL); break; }
        if (cur_fn && cur_fn->is_ctor) {
            if (n->a) { diag_error(n->loc, "constructors do not return a value"); ex(&n->a, NULL); }
            break;
        }
        if (cur_ret->kind == TY_VOID) {
            if (n->a) { diag_error(n->loc, "the function is void and cannot return a value"); ex(&n->a, NULL); }
        } else if (!n->a) {
            diag_error(n->loc, "the function must return a value of type '%s'", type_str(cur_ret));
        } else {
            ex(&n->a, cur_ret);
            n->a = coerce(n->a, cur_ret, "return");
        }
        break;
    }
    case N_BREAK: if (!loop_depth && !switch_depth) diag_error(n->loc, "'break' outside a loop or switch"); break;
    case N_CONTINUE: if (!loop_depth) diag_error(n->loc, "'continue' outside a loop"); break;
    case N_TRY: {
        int saved = next_slot;
        n->slot = next_slot;
        for (int i = 0; i < 4; i++) alloc_slot();
        stmt(&n->a);
        for (int i = 0; i < n->list.n; i++) {
            Node *c = n->list.v[i];
            Type *ct = resolve_type(c->type);
            if (!is_err(ct) && !(ct->kind == TY_CLASS && exception_cls && is_subclass(ct->cls, exception_cls))) {
                diag_error(c->loc, "catch requires a class derived from Exception, got '%s'", type_str(ct));
                ct = TY_error_t;
            }
            c->type = ct;
            push_scope();
            int s2 = next_slot;
            c->sym = declare_var(c->name, ct, false, c->loc);
            stmt(&c->a);
            next_slot = s2;
            pop_scope();
        }
        if (n->b) stmt(&n->b);
        next_slot = saved;
        break;
    }
    case N_THROW: {
        Type *t = ex(&n->a, NULL);
        if (!is_err(t) && !(t->kind == TY_CLASS && exception_cls && is_subclass(t->cls, exception_cls)))
            diag_error(n->loc, "throw requires an object of a class derived from Exception, got '%s'", type_str(t));
        break;
    }
    case N_SWITCH: {
        Type *t = ex(&n->a, NULL);
        bool isstr = !is_err(t) && t->kind == TY_STRING;
        if (!is_err(t) && !isstr && !type_is_integral(t))
            diag_error(n->a->loc, "'switch' accepts int, char, bool or String, got '%s'", type_str(t));
        int saved = next_slot;
        n->slot = alloc_slot();
        push_scope();
        switch_depth++;
        bool seen_default = false;
        Vec seen_vals = {0};
        for (int i = 0; i < n->list.n; i++) {
            Node *c = n->list.v[i];
            if (!c->a) {
                if (seen_default) diag_error(c->loc, "'switch' with more than one 'default'");
                seen_default = true;
            } else {
                Type *lt = ex(&c->a, t);
                if (!is_err(lt) && !is_err(t)) {
                    int64_t v;
                    if (isstr) {
                        if (c->a->kind != N_STR) diag_error(c->a->loc, "String 'case' labels must be text literals");
                        else {
                            for (int k = 0; k < seen_vals.n; k++) { Node *o = seen_vals.v[k]; if (o->ival == c->a->ival && !memcmp(o->name, c->a->name, (size_t)o->ival)) { diag_error(c->a->loc, "duplicate 'case' label"); break; } }
                            vec_push(&seen_vals, c->a);
                        }
                    } else if (!const_label(c->a, &v)) {
                        diag_error(c->a->loc, "the 'case' label must be a constant (number, character or true/false)");
                    } else {
                        c->ival = v;
                        for (int k = 0; k < seen_vals.n; k++) if (((Node *)seen_vals.v[k])->ival == v) { diag_error(c->a->loc, "duplicate 'case' label (%lld)", (long long)v); break; }
                        vec_push(&seen_vals, c);
                    }
                }
            }
            for (int k = 0; k < c->list.n; k++) stmt((Node **)&c->list.v[k]);
        }
        switch_depth--;
        pop_scope();
        next_slot = saved;
        break;
    }
    case N_CLASSSTMT:
        diag_error(n->a->loc, "class '%s' is inside a function: classes cannot be declared inside functions", n->a->name);
        diag_note(n->a->loc, "move the class outside of 'void main()'; declare classes at the top level of the file");
        break;
    default:
        diag_error(n->loc, "internal error: unexpected statement (%d)", n->kind);
    }
}

/* ====================== functions and program ====================== */
static void analyze_func(FuncInfo *f) {
    cur_fn = f;
    cur_cls = f->owner;
    cur_ret = f->ret;
    in_global_init = false;
    loop_depth = 0;
    switch_depth = 0;
    scope = NULL;
    push_scope();
    next_slot = 1;                      /* slot 0 reserved: keeps the return value during 'finally' */
    f->frame_slots = 1;
    for (int i = 0; i < f->params.n; i++) vec_push(&scope->syms, f->params.v[i]);
    allowed_super = NULL;
    if (f->is_ctor && f->body && f->body->list.n > 0) {
        Node *first = f->body->list.v[0];
        if (first->kind == N_EXPRSTMT && first->a->kind == N_CALL && first->a->a->kind == N_SUPER) allowed_super = first->a;
    }
    if (f->body) stmt(&f->body);
    if (f->is_ctor && f->owner->parent && !allowed_super) {
        FuncInfo *pc = NULL;
        for (int i = 0; i < f->owner->parent->ctors.n; i++)
            if (((FuncInfo *)f->owner->parent->ctors.v[i])->nargs == 0) pc = f->owner->parent->ctors.v[i];
        if (!pc) {
            diag_error(f->loc, "superclass '%s' has no no-argument constructor; call super(...) explicitly at the start of the constructor", f->owner->parent->name);
        }
    }
    if (!f->is_ctor && f->ret->kind != TY_VOID && !is_err(f->ret) && f->body && !always_returns(f->body))
        diag_error(f->loc, "function '%s' must return a value of type '%s' on all paths", f->name, type_str(f->ret));
    scope = NULL;
    cur_fn = NULL;
    allowed_super = NULL;
}

static bool g_force_strict;
void sema_set_strict(bool on) { g_force_strict = on; }

Program *sema_analyze(Vec prelude_items, Vec user_items) {
    prog = xcalloc(sizeof(Program));
    prog->strict = g_force_strict;
    Vec items = {0};
    for (int i = 0; i < prelude_items.n; i++) vec_push(&items, prelude_items.v[i]);
    for (int i = 0; i < user_items.n; i++) vec_push(&items, user_items.v[i]);
    global_scope = xcalloc(sizeof(Scope));

    for (int i = 0; i < items.n; i++) {
        Node *d = items.v[i];
        if (d->kind == N_DIRECTIVE && !strcmp(d->name, "NOT_GARBAGE")) prog->no_gc = true;
        if (d->kind == N_DIRECTIVE && !strcmp(d->name, "STRICT")) prog->strict = true;
    }

    /* 0) enums */
    for (int i = 0; i < items.n; i++) {
        Node *d = items.v[i];
        if (d->kind != N_ENUM) continue;
        if (find_enum(d->name)) diag_error(d->loc, "enum '%s' is already declared", d->name);
        else vec_push(&all_enums, d);
        for (int a = 0; a < d->list.n; a++)
            for (int b = a + 1; b < d->list.n; b++)
                if (!strcmp(((Node *)d->list.v[a])->name, ((Node *)d->list.v[b])->name))
                    diag_error(((Node *)d->list.v[b])->loc, "constant '%s' repeated in enum '%s'", ((Node *)d->list.v[b])->name, d->name);
    }
    /* 1) classes */
    declare_classes(&items);
    link_parents();
    order_classes();
    exception_cls = find_class("Exception");

    /* diretivas de pacote */
    for (int i = 0; i < items.n; i++) {
        Node *d = items.v[i];
        if (d->kind != N_DIRECTIVE || strncmp(d->name, "definePack", 10) != 0) continue;
        for (int j = 0; j < d->list.n; j++) {
            Node *id = d->list.v[j];
            ClassInfo *c = find_class(id->name);
            if (!c) diag_error(id->loc, "#definePackage: class '%s' does not exist", id->name);
            else { c->exported = true; vec_push(&prog->packages, c->name); }
        }
    }

    /* 2) global functions */
    for (int i = 0; i < items.n; i++) {
        Node *d = items.v[i];
        if (d->kind != N_FUNC) continue;
        if (!in_prelude(d->loc) && strncmp(d->name, "__", 2) == 0) diag_error(d->loc, "names starting with '__' are reserved for the runtime");
        FuncInfo *f = build_func(d, NULL);
        d->fn = f;
        FuncInfo *dup = find_function(f->name, f->nargs);
        if (dup) {
            diag_error(d->loc, "function '%s' with %d parameter(s) is already declared (line %d)", f->name, f->nargs, dup->loc.line);
            continue;
        }
        vec_push(&all_funcs, f);
        vec_push(&prog->funcs, f);
        if (!f->is_prelude && !strcmp(f->name, "main")) prog->main_fn = f;
    }
    for (int i = 0; i < prog->funcs.n; i++) {          /* also exposes runtime functions by name */
        FuncInfo *f = prog->funcs.v[i];
        (void)f;
    }

    /* 3) top-level statements -> __globals_init */
    FuncInfo *gi = new_func("__globals_init", TY_void_t, NULL, (Loc){0});
    gi->is_prelude = true;
    Node *gbody = node_new(N_BLOCK, (Loc){0});
    gbody->flags |= NF_NOSCOPE;
    for (int i = 0; i < items.n; i++) {
        Node *d = items.v[i];
        switch (d->kind) {
        case N_CLASS: case N_FUNC: case N_DIRECTIVE: case N_ENUM: break;
        default: vec_push(&gbody->list, d);
        }
    }
    for (int i = static_inits.n - 1; i >= 0; i--) {              /* statics are initialized before the top-level code */
        vec_push(&gbody->list, NULL);
        memmove(&gbody->list.v[1], &gbody->list.v[0], sizeof(void *) * (size_t)(gbody->list.n - 1));
        gbody->list.v[0] = static_inits.v[i];
    }
    gi->body = gbody;
    prog->globals_init = gi;
    cur_fn = gi; cur_cls = NULL; cur_ret = TY_void_t;
    in_global_init = true;
    scope = global_scope;
    next_slot = 1;
    gi->frame_slots = 1;
    for (int i = 0; i < gbody->list.n; i++) stmt((Node **)&gbody->list.v[i]);
    in_global_init = false;
    scope = NULL;
    cur_fn = NULL;
    vec_push(&prog->funcs, gi);

    /* 4) field initializers and bodies */
    for (int i = 0; i < prog->classes.n; i++) {
        ClassInfo *c = prog->classes.v[i];
        cur_cls = c; cur_fn = NULL; cur_ret = TY_void_t;
        scope = NULL; push_scope(); next_slot = 1;
        for (int j = 0; j < c->decl->list.n; j++) {
            Node *m = c->decl->list.v[j];
            if (m->kind != N_FIELD || !m->a || !m->sym || (m->flags & NF_STATIC)) continue;
            ex(&m->a, m->sym->type);
            m->a = coerce(m->a, m->sym->type, xsprintf("initialization of field '%s'", m->name));
        }
        scope = NULL;
        cur_cls = NULL;
    }
    for (int i = 0; i < prog->funcs.n; i++) {
        FuncInfo *f = prog->funcs.v[i];
        if (f == gi) continue;
        analyze_func(f);
    }

    for (int i = 0; i < prog->classes.n; i++) {                  /* offsets of the static fields */
        ClassInfo *c = prog->classes.v[i];
        for (int j = 0; j < c->statics.n; j++) {
            Symbol *y = c->statics.v[j];
            y->offset = prog->globals_size;
            prog->globals_size += 8;
        }
    }
    for (int i = 0; i < implicit_syms.n; i++) {
        Symbol *y = implicit_syms.v[i];
        if (!y->used) diag_warning(y->loc, "variable '%s' is assigned but never read", y->name);
    }
    if (!prog->main_fn) {
        diag_error((Loc){0}, "function 'main' not found: every program needs 'void main() { ... }'");
    } else if (prog->main_fn->nargs != 0 || (prog->main_fn->ret->kind != TY_VOID && prog->main_fn->ret->kind != TY_INT)) {
        diag_error(prog->main_fn->loc, "'main' must be 'void main()' or 'int main()', with no parameters");
    }
    return prog;
}
