/*
 * codegen.h - x86-64 machine code generation from the annotated AST.
 */
#ifndef ADAC_CODEGEN_H
#define ADAC_CODEGEN_H
#include "ast.h"

/* generates the PE executable at 'out_path' */
void codegen_emit(Program *prog, const char *out_path);

#endif
