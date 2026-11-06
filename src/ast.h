/*
 * ast.h - Types, symbols and abstract syntax tree (AST) of the compiler.
 *
 * The parser builds the AST; semantic analysis annotates it (types, symbols,
 * implicit conversions) and rewrites some constructs (print, string concatenation...)
 * into direct calls to the runtime; the code generator consumes it.
 */
#ifndef ADAC_AST_H
#define ADAC_AST_H
#include "common.h"
#include "lexer.h"

/* ====================== Tipos ====================== */
typedef enum {
    TY_VOID, TY_INT, TY_FLOAT, TY_BOOL, TY_CHAR, TY_STRING,
    TY_NULL,      /* type of the null literal */
    TY_PTR_ANY,   /* 'ptr': generic pointer (void*) */
    TY_PTR,       /* T*  */
    TY_ARRAY,     /* T[] */
    TY_CLASS,
    TY_ERROR      /* recovery type: avoids cascading errors */
} TyKind;

struct ClassInfo;
typedef struct Type {
    TyKind kind;
    struct Type *elem;        /* TY_PTR / TY_ARRAY */
    char *name;               /* TY_CLASS: name (resolved into .cls) */
    struct ClassInfo *cls;
    Loc loc;
} Type;

extern Type *TY_void_t, *TY_int_t, *TY_float_t, *TY_bool_t, *TY_char_t,
            *TY_string_t, *TY_null_t, *TY_ptrany_t, *TY_error_t;

Type *type_ptr_to(Type *elem);
Type *type_array_of(Type *elem);
Type *type_class_named(const char *name, Loc loc);   /* not resolved yet */
Type *type_class_of(struct ClassInfo *cls);
bool  type_equal(Type *a, Type *b);
bool  type_is_numeric(Type *t);        /* int, char, float */
bool  type_is_integral(Type *t);       /* int, char, bool */
bool  type_is_reference(Type *t);      /* aceita null */
const char *type_str(Type *t);

/* ====================== Simbolos ====================== */
enum { ACC_PACKAGE = 0, ACC_PUBLIC, ACC_PRIVATE, ACC_PROTECTED };

typedef enum { SYM_LOCAL, SYM_PARAM, SYM_GLOBAL, SYM_FIELD } SymKind;

typedef struct Symbol {
    SymKind kind;
    char   *name;
    Type   *type;
    bool    is_const;
    int     offset;     /* LOCAL/PARAM: offset from rbp; GLOBAL: offset in .data; FIELD: offset in the object */
    int     access;
    bool    used, implicit;   /* for unused-variable / declared-by-assignment warnings */
    struct ClassInfo *owner;   /* FIELD */
    Loc     loc;
} Symbol;

struct Node;

typedef struct FuncInfo {
    char *name;
    Type *ret;
    Vec   params;            /* Symbol* (includes 'this' as the 1st one in methods) */
    int   nparams;           /* total pushed (with this) */
    int   nargs;             /* declared arguments (without this) */
    struct ClassInfo *owner; /* NULL for a global function */
    bool  is_ctor;
    bool  is_static, is_abstract;
    int   mid;               /* global id of the signature (interface dispatch) */
    bool  is_virtual;
    int   vslot;             /* indice na vtable */
    int   access;
    struct Node *decl;       /* N_FUNC (NULL em ctor sintetico) */
    struct Node *body;       /* body to generate */
    int   label;             /* code label (codegen) */
    int   frame_slots;       /* 8-byte slots used in the frame */
    bool  is_prelude;
    Loc   loc;
} FuncInfo;

typedef struct ClassInfo {
    char *name;
    struct ClassInfo *parent;
    struct Node *decl;
    Vec   fields;            /* Symbol* proprios */
    Vec   methods;           /* own FuncInfo* (without ctor) */
    Vec   ctors;             /* FuncInfo* (always at least one, possibly synthetic) */
    Vec   vtable;            /* FuncInfo* (vtable completa) */
    int   nfields;           /* total, including inherited ones */
    int   data_off;          /* offset da vtable em .data */
    bool  exported;
    bool  is_abstract, is_interface;
    Vec   ifaces;            /* ClassInfo* implemented / extended (interfaces) */
    Vec   statics;           /* Symbol* of static fields */
    bool  resolved;
    bool  is_prelude;
    Loc   loc;
} ClassInfo;

/* ====================== AST ====================== */
typedef enum {
    /* expressions */
    N_INT, N_FLOAT, N_STR, N_CHAR, N_BOOL, N_NULL, N_THIS, N_SUPER,
    N_IDENT, N_UNARY, N_BINARY, N_ASSIGN, N_INCDEC, N_CALL, N_MEMBER, N_INDEX,
    N_NEW, N_NEWARRAY, N_CAST, N_COND, N_SIZEOF, N_INSTANCEOF, N_ARRLIT,
    N_CALLFN,    /* (sema) direct/virtual call: fn, a=receiver (opt), list=args */
    N_BUILTIN,   /* (sema) compiler intrinsic: op = BI_* */
    N_SEQ,       /* (sema) evaluates each item of 'list' in order; void value */
    /* statements */
    N_BLOCK, N_VAR, N_EXPRSTMT, N_IF, N_WHILE, N_DO, N_FOR, N_RETURN,
    N_BREAK, N_CONTINUE, N_TRY, N_CATCH, N_THROW, N_CLASSSTMT, N_EMPTY,
    N_SWITCH,    /* a=expr, list=N_CASE, slot=temp */
    N_CASE,      /* a=label (NULL=default), list=statements, ival=constant value */
    /* declaracoes */
    N_CLASS, N_FUNC, N_FIELD, N_PARAM, N_DIRECTIVE,
    N_ENUM       /* name, list=N_IDENT(ival=value) */
} NodeKind;

enum {  /* node flags */
    NF_CONST      = 1 << 0,   /* const declaration */
    NF_POST       = 1 << 1,   /* x++ (postfix) */
    NF_NOSCOPE    = 1 << 2,   /* block that does not open a scope (multiple declarators) */
    NF_CTOR       = 1 << 3,
    NF_IMPLICIT_THIS = 1 << 4,/* IDENT that is a field accessed via implicit this */
    NF_VIRTUAL    = 1 << 5,   /* CALLFN with virtual dispatch */
    NF_ARRLEN     = 1 << 6,   /* MEMBER: array.length */
    NF_FOR_PREDECL= 1 << 7,   /* for(_; ...) : variable that already exists */
    NF_FOR_IMPLICIT = 1 << 8, /* for(int _; ...) : declares the condition's variable */
    NF_IMPLICIT   = 1 << 9,   /* CAST inserted by the compiler */
    NF_SUPERCALL  = 1 << 10,  /* CALLFN to a parent method (static dispatch) */
    NF_INFERRED   = 1 << 11,  /* VAR declared without a type (inferred) */
    NF_GLOBALINIT = 1 << 12,  /* VAR global */
    NF_STATIC     = 1 << 13,
    NF_ABSTRACT   = 1 << 14,
    NF_INTERFACE  = 1 << 15,
};

/* intrinsicos (N_BUILTIN) */
enum {
    BI_ARRLEN = 1, BI_SQRT, BI_WINAPI, BI_PEEK8, BI_POKE8, BI_PEEK64, BI_POKE64,
    BI_SP, BI_GBEGIN, BI_GEND, BI_MEMCPY
};

typedef struct Node Node;
struct Node {
    NodeKind kind;
    Loc      loc;
    Type    *type;      /* expression type (sema) / declared type (VAR, FUNC=return, FIELD, PARAM, CATCH, CAST) */
    Node    *a, *b, *c, *d;
    Vec      list;
    char    *name;
    int64_t  ival;
    double   fval;
    int      op;        /* operator token, or BI_* */
    int      flags;
    int      access;
    Symbol  *sym;
    FuncInfo *fn;
    ClassInfo *cls;
    int      slot;      /* N_TRY: first slot of the exception record */
};

Node *node_new(NodeKind k, Loc loc);

/* ====================== Programa ====================== */
typedef struct Program {
    Vec   classes;       /* ClassInfo* (parents before children) */
    Vec   funcs;         /* FuncInfo*: everything that generates code */
    Vec   globals;       /* Symbol* */
    int   globals_size;  /* bytes */
    FuncInfo *main_fn;
    FuncInfo *globals_init;
    bool  no_gc;
    bool  strict;        /* #STRICT / --strict: declaration by assignment becomes an error */
    Vec   packages;      /* names listed in #definePackage */
} Program;

#endif
