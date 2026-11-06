/*
 * lexer.h - Lexical analysis of the adaC language.
 */
#ifndef ADAC_LEXER_H
#define ADAC_LEXER_H
#include "common.h"

typedef enum {
    TK_EOF, TK_IDENT, TK_INT, TK_FLOAT, TK_STRING, TK_CHAR, TK_DIRECTIVE,
    /* keywords */
    KW_IF, KW_ELSE, KW_WHILE, KW_FOR, KW_DO, KW_RETURN, KW_BREAK, KW_CONTINUE,
    KW_CLASS, KW_EXTENDS, KW_NEW, KW_THIS, KW_SUPER, KW_CONST,
    KW_TRY, KW_CATCH, KW_FINALLY, KW_THROW, KW_TRUE, KW_FALSE, KW_NULL,
    KW_PUBLIC, KW_PRIVATE, KW_PROTECTED, KW_PACKAGE, KW_SIZEOF,
    KW_SWITCH, KW_CASE, KW_DEFAULT, KW_INSTANCEOF, KW_STATIC, KW_ABSTRACT, KW_INTERFACE,
    /* primitive types */
    KW_INT, KW_LONG, KW_FLOAT, KW_DOUBLE, KW_BOOL, KW_CHAR, KW_STRING, KW_VOID, KW_PTR,
    /* punctuation and operators */
    P_LPAREN, P_RPAREN, P_LBRACE, P_RBRACE, P_LBRACK, P_RBRACK,
    P_SEMI, P_COMMA, P_DOT, P_COLON, P_QUESTION,
    P_PLUS, P_MINUS, P_STAR, P_SLASH, P_PERCENT,
    P_AMP, P_PIPE, P_CARET, P_TILDE, P_BANG,
    P_ASSIGN, P_EQ, P_NE, P_LT, P_LE, P_GT, P_GE,
    P_ANDAND, P_OROR, P_SHL, P_SHR, P_PLUSPLUS, P_MINUSMINUS,
    P_PLUSEQ, P_MINUSEQ, P_STAREQ, P_SLASHEQ, P_PERCENTEQ,
    P_AMPEQ, P_PIPEEQ, P_CARETEQ, P_SHLEQ, P_SHREQ,
    TK__COUNT
} TokKind;

typedef struct Token {
    TokKind kind;
    Loc     loc;
    char   *text;     /* identifier, directive name, string bytes */
    int     len;      /* tamanho de 'text' (strings podem conter \0) */
    int64_t ival;
    double  fval;
} Token;

typedef struct TokenList {
    Token *t;
    int n;
} TokenList;

TokenList lex(Source *src);
const char *token_kind_name(TokKind k);   /* readable name for messages */

#endif
