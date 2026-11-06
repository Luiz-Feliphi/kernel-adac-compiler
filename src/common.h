/*
 * common.h - Types and utilities shared by the whole adaC compiler.
 */
#ifndef ADAC_COMMON_H
#define ADAC_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

/* ---------- Source and location ---------- */
typedef struct Source {
    char *name;        /* file name (for messages) */
    char *text;        /* contents, terminated by '\0' */
    int   len;
    bool  is_prelude;  /* true for the embedded runtime */
} Source;

typedef struct Loc {
    Source *src;
    int line, col;     /* 1-based */
} Loc;

/* ---------- Vetor dinamico de ponteiros ---------- */
typedef struct Vec {
    void **v;
    int n, cap;
} Vec;

void  vec_push(Vec *vec, void *item);

/* ---------- Memoria / strings ---------- */
void *xcalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xsprintf(const char *fmt, ...);

/* ---------- Diagnosticos ---------- */
void diag_error(Loc loc, const char *fmt, ...);
void diag_warning(Loc loc, const char *fmt, ...);
void diag_note(Loc loc, const char *fmt, ...);
void diag_fatal(const char *fmt, ...);     /* error without a location, aborts */
int  diag_error_count(void);
int  diag_warning_count(void);
void diag_set_color(bool on);

/* ---------- Arquivos ---------- */
Source *source_from_file(const char *path);
Source *source_from_string(const char *name, const char *text, bool is_prelude);

#endif
