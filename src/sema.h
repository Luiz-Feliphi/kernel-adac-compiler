/*
 * sema.h - Semantic analysis: scopes, types, classes, checks and
 *          rewriting of high-level constructs into runtime calls.
 */
#ifndef ADAC_SEMA_H
#define ADAC_SEMA_H
#include "ast.h"

/* prelude_items / user_items: lists of top-level items (Node*) */
Program *sema_analyze(Vec prelude_items, Vec user_items);

/* looks up a global runtime function (e.g. "__alloc") */
FuncInfo *rt_func(const char *name);
void sema_set_strict(bool on);

#endif
