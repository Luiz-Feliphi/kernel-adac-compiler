/*
 * util.c - Memoria, vetores, leitura de arquivos e sistema de diagnosticos.
 */
#include "common.h"
#ifndef _WIN32
#include <unistd.h>
#endif

/* ---------------- memoria ---------------- */
void *xcalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) diag_fatal("out of memory");
    return p;
}
void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) diag_fatal("out of memory");
    return p;
}
char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *r = xcalloc(n + 1);
    memcpy(r, s, n);
    return r;
}
char *xstrndup(const char *s, size_t n) {
    char *r = xcalloc(n + 1);
    memcpy(r, s, n);
    return r;
}
char *xsprintf(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *r = xcalloc((size_t)n + 1);
    vsnprintf(r, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return r;
}

void vec_push(Vec *vec, void *item) {
    if (vec->n == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->v = xrealloc(vec->v, sizeof(void *) * (size_t)vec->cap);
    }
    vec->v[vec->n++] = item;
}

/* ---------------- sources ---------------- */
Source *source_from_string(const char *name, const char *text, bool is_prelude) {
    Source *s = xcalloc(sizeof(Source));
    s->name = xstrdup(name);
    s->text = xstrdup(text);
    s->len = (int)strlen(text);
    s->is_prelude = is_prelude;
    return s;
}

Source *source_from_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) diag_fatal("could not open '%s'", path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xcalloc((size_t)sz + 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        diag_fatal("error reading '%s'", path);
    }
    fclose(f);
    Source *s = xcalloc(sizeof(Source));
    s->name = xstrdup(path);
    s->text = buf;
    s->len = (int)sz;
    /* ignora BOM UTF-8 */
    if (s->len >= 3 && (uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB && (uint8_t)buf[2] == 0xBF) {
        s->text += 3;
        s->len -= 3;
    }
    return s;
}

/* ---------------- diagnosticos ---------------- */
static int  g_errors = 0, g_warnings = 0;
static bool g_color = false;

void diag_set_color(bool on) { g_color = on; }
int  diag_error_count(void) { return g_errors; }
int  diag_warning_count(void) { return g_warnings; }

#define C_RED   (g_color ? "\x1b[1;31m" : "")
#define C_YEL   (g_color ? "\x1b[1;33m" : "")
#define C_CYAN  (g_color ? "\x1b[1;36m" : "")
#define C_BOLD  (g_color ? "\x1b[1m"    : "")
#define C_RST   (g_color ? "\x1b[0m"    : "")

/* prints the source line with a ^ under the problem column */
static void print_snippet(Loc loc) {
    if (!loc.src || loc.line <= 0) return;
    const char *p = loc.src->text;
    int line = 1;
    while (*p && line < loc.line) {
        if (*p == '\n') line++;
        p++;
    }
    const char *e = p;
    while (*e && *e != '\n' && *e != '\r') e++;
    fprintf(stderr, " %5d | %.*s\n", loc.line, (int)(e - p), p);
    fprintf(stderr, " %5s | ", "");
    for (int i = 1; i < loc.col && p + i - 1 < e; i++) fputc(p[i - 1] == '\t' ? '\t' : ' ', stderr);
    fprintf(stderr, "%s^%s\n", C_RED, C_RST);
}

static void vreport(const char *kind, const char *color, Loc loc, const char *fmt, va_list ap, bool snippet) {
    if (loc.src) fprintf(stderr, "%s%s:%d:%d:%s ", C_BOLD, loc.src->name, loc.line, loc.col, C_RST);
    fprintf(stderr, "%s%s:%s ", color, kind, C_RST);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    if (snippet) print_snippet(loc);
}

void diag_error(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport("error", C_RED, loc, fmt, ap, true);
    va_end(ap);
    if (++g_errors >= 50) diag_fatal("too many errors; aborting");
}
void diag_warning(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport("warning", C_YEL, loc, fmt, ap, true);
    va_end(ap);
    g_warnings++;
}
void diag_note(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport("note", C_CYAN, loc, fmt, ap, true);
    va_end(ap);
}
void diag_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%sadac: fatal error:%s ", C_RED, C_RST);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}
