/*
 * types.c - Constructors and predicates of the type system.
 */
#include "ast.h"

static Type mk(TyKind k) { Type t = {0}; t.kind = k; return t; }
static Type *alloc_type(TyKind k) { Type *t = xcalloc(sizeof(Type)); t->kind = k; return t; }

Type *TY_void_t, *TY_int_t, *TY_float_t, *TY_bool_t, *TY_char_t,
     *TY_string_t, *TY_null_t, *TY_ptrany_t, *TY_error_t;

static void init_types(void) __attribute__((constructor));
static void init_types(void) {
    (void)mk;
    TY_void_t = alloc_type(TY_VOID);   TY_int_t = alloc_type(TY_INT);
    TY_float_t = alloc_type(TY_FLOAT); TY_bool_t = alloc_type(TY_BOOL);
    TY_char_t = alloc_type(TY_CHAR);   TY_string_t = alloc_type(TY_STRING);
    TY_null_t = alloc_type(TY_NULL);   TY_ptrany_t = alloc_type(TY_PTR_ANY);
    TY_error_t = alloc_type(TY_ERROR);
}

Type *type_ptr_to(Type *elem) { Type *t = alloc_type(TY_PTR); t->elem = elem; return t; }
Type *type_array_of(Type *elem) { Type *t = alloc_type(TY_ARRAY); t->elem = elem; return t; }
Type *type_class_named(const char *name, Loc loc) {
    Type *t = alloc_type(TY_CLASS);
    t->name = xstrdup(name);
    t->loc = loc;
    return t;
}
Type *type_class_of(ClassInfo *cls) {
    Type *t = alloc_type(TY_CLASS);
    t->name = cls->name;
    t->cls = cls;
    return t;
}

bool type_equal(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
    case TY_PTR: case TY_ARRAY: return type_equal(a->elem, b->elem);
    case TY_CLASS:
        if (a->cls ? a->cls != b->cls : !(b->cls == NULL && strcmp(a->name, b->name) == 0)) return false;
        if (a->elem || b->elem) return a->elem && b->elem && type_equal(a->elem, b->elem);
        return true;
    default: return true;
    }
}

bool type_is_numeric(Type *t) { return t->kind == TY_INT || t->kind == TY_FLOAT || t->kind == TY_CHAR; }
bool type_is_integral(Type *t) { return t->kind == TY_INT || t->kind == TY_CHAR || t->kind == TY_BOOL; }
bool type_is_reference(Type *t) {
    switch (t->kind) {
    case TY_STRING: case TY_PTR: case TY_PTR_ANY: case TY_ARRAY: case TY_CLASS: case TY_NULL: return true;
    default: return false;
    }
}

const char *type_str(Type *t) {
    if (!t) return "?";
    switch (t->kind) {
    case TY_VOID: return "void";
    case TY_INT: return "int";
    case TY_FLOAT: return "float";
    case TY_BOOL: return "bool";
    case TY_CHAR: return "char";
    case TY_STRING: return "String";
    case TY_NULL: return "null";
    case TY_PTR_ANY: return "ptr";
    case TY_PTR: return xsprintf("%s*", type_str(t->elem));
    case TY_ARRAY: return xsprintf("%s[]", type_str(t->elem));
    case TY_CLASS:
        if (t->elem) return !strcmp(t->name, "Map") ? xsprintf("Map<String,%s>", type_str(t->elem)) : xsprintf("%s<%s>", t->name, type_str(t->elem));
        return t->name;
    default: return "<erro>";
    }
}

Node *node_new(NodeKind k, Loc loc) {
    Node *n = xcalloc(sizeof(Node));
    n->kind = k;
    n->loc = loc;
    return n;
}
