/*
 * parser.h - Syntax analysis: tokens -> AST.
 */
#ifndef ADAC_PARSER_H
#define ADAC_PARSER_H
#include "ast.h"

/* Pre-scans for 'class NAME' so the parser can tell casts and types apart. */
void parser_prescan(TokenList *tl);

/* Parses the whole file. Returns the list of top-level items (Node*). */
Vec parse_program(TokenList *tl);

#endif
