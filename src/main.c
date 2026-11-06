/*
 * main.c - adaC compiler driver (adac).
 *
 *   adac programa.adac [-o saida.exe] [--check] [--tokens] [--dump-prelude]
 *
 * Pipeline:  source -> tokens -> AST -> semantic analysis -> x86-64 -> PE (.exe)
 */
#include "common.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "codegen.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

/* wall-clock time in milliseconds (high resolution) */
static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

extern const char *ADAC_RT_CORE, *ADAC_RT_GC, *ADAC_RT_MANUAL;

#define VERSION "1.0"

static void usage(void) {
    fprintf(stderr,
        "adac %s - compiler for the adaC language (generates 64-bit Windows .exe files)\n\n"
        "usage: adac <file.adac> [options]\n"
        "  -o <file>         output executable name (default: <file>.exe)\n"
        "  --check           only check the program (does not generate the .exe)\n"
        "  --strict          variables must be declared (error for 'x = 1' without declaration; or use #STRICT)\n"
        "  --tokens          print the tokens and exit\n"
        "  --dump-prelude    print the embedded runtime and exit (add --manual for manual-memory mode)\n"
        "  --manual          with --dump-prelude, show the #NOT_GARBAGE runtime\n"
        "  --color / --no-color\n"
        "  -v                show information about the compiler stages\n"
        "  -h, --help        show this help\n", VERSION);
}

static char *default_output(const char *in) {
    const char *slash = strrchr(in, '/');
    const char *bs = strrchr(in, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
    const char *base = slash ? slash + 1 : in;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    char *out = xcalloc(n + 5);
    memcpy(out, base, n);
    strcat(out, ".exe");
    return out;
}

/* ---------------- modules (import "file.adac";) ---------------- */
typedef struct { Source *src; TokenList toks; } Module;
static Vec modules;     /* Module*, dependencies before the importer */
static Vec seen_paths;

static char *dir_of(const char *path) {
    const char *s1 = strrchr(path, '/'), *s2 = strrchr(path, '\\');
    const char *sl = s1 > s2 ? s1 : s2;
    if (!sl) return xstrdup("");
    return xstrndup(path, (size_t)(sl - path + 1));
}

static void load_module(const char *path) {
    for (int i = 0; i < seen_paths.n; i++) if (!strcmp(seen_paths.v[i], path)) return;
    vec_push(&seen_paths, xstrdup(path));
    Module *m = xcalloc(sizeof(Module));
    m->src = source_from_file(path);
    m->toks = lex(m->src);
    char *dir = dir_of(path);
    for (int i = 0; i + 1 < m->toks.n; i++) {
        Token *t = &m->toks.t[i];
        if (t->kind == TK_IDENT && !strcmp(t->text, "import") && m->toks.t[i + 1].kind == TK_STRING) {
            const char *rel = m->toks.t[i + 1].text;
            bool abs = rel[0] == '/' || rel[0] == '\\' || (rel[0] && rel[1] == ':');
            load_module(abs ? rel : xsprintf("%s%s", dir, rel));
        }
    }
    vec_push(&modules, m);
}

static bool has_directive(TokenList *tl, const char *a, const char *b) {
    for (int i = 0; i < tl->n; i++)
        if (tl->t[i].kind == TK_DIRECTIVE && (!strcmp(tl->t[i].text, a) || !strcmp(tl->t[i].text, b))) return true;
    return false;
}

int main(int argc, char **argv) {
    double t_start = now_ms();
    const char *input = NULL, *output = NULL;
    bool check = false, tokens = false, dump = false, manual_dump = false, verbose = false;
    bool color =
#ifndef _WIN32
        isatty(2);
#else
        false;
#endif
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-o")) { if (++i >= argc) diag_fatal("option -o requires a file name"); output = argv[i]; }
        else if (!strcmp(a, "--check")) check = true;
        else if (!strcmp(a, "--strict")) sema_set_strict(true);
        else if (!strcmp(a, "--tokens")) tokens = true;
        else if (!strcmp(a, "--dump-prelude")) dump = true;
        else if (!strcmp(a, "--manual")) manual_dump = true;
        else if (!strcmp(a, "--color")) color = true;
        else if (!strcmp(a, "--no-color")) color = false;
        else if (!strcmp(a, "-v")) verbose = true;
        else if (a[0] == '-') { fprintf(stderr, "adac: unknown option '%s'\n", a); usage(); return 2; }
        else if (!input) input = a;
        else diag_fatal("more than one input file ('%s' and '%s')", input, a);
    }
    diag_set_color(color);
    if (dump) {
        fputs(ADAC_RT_CORE, stdout);
        fputs(manual_dump ? ADAC_RT_MANUAL : ADAC_RT_GC, stdout);
        return 0;
    }
    if (!input) { usage(); return 2; }

    /* 1) source and lexer */
    load_module(input);
    TokenList utoks = ((Module *)modules.v[modules.n - 1])->toks;
    if (tokens) {
        for (int i = 0; i < utoks.n; i++) {
            Token *t = &utoks.t[i];
            printf("%4d:%-3d %-16s", t->loc.line, t->loc.col, token_kind_name(t->kind));
            if (t->kind == TK_IDENT || t->kind == TK_DIRECTIVE || t->kind == TK_STRING) printf(" \"%s\"", t->text);
            else if (t->kind == TK_INT || t->kind == TK_CHAR) printf(" %lld", (long long)t->ival);
            else if (t->kind == TK_FLOAT) printf(" %g", t->fval);
            putchar('\n');
        }
        return diag_error_count() ? 1 : 0;
    }

    /* 2) memory mode and runtime */
    bool manual = false;
    for (int i = 0; i < modules.n; i++) if (has_directive(&((Module *)modules.v[i])->toks, "NOT_GARBAGE", "NOT_GARGBAGE")) manual = true;
    char *rt_text = xsprintf("%s%s", ADAC_RT_CORE, manual ? ADAC_RT_MANUAL : ADAC_RT_GC);
    Source *rt_src = source_from_string("<runtime>", rt_text, true);
    TokenList rtoks = lex(rt_src);
    if (diag_error_count()) diag_fatal("internal error in the runtime lexer");

    /* 3) parser */
    parser_prescan(&rtoks);
    for (int i = 0; i < modules.n; i++) parser_prescan(&((Module *)modules.v[i])->toks);
    Vec rt_items = parse_program(&rtoks);
    if (diag_error_count()) diag_fatal("internal error: the runtime does not parse");
    Vec items = {0};
    for (int i = 0; i < modules.n; i++) {
        Vec mi = parse_program(&((Module *)modules.v[i])->toks);
        for (int k = 0; k < mi.n; k++) vec_push(&items, mi.v[k]);
    }
    if (verbose) fprintf(stderr, "[adac] %d top-level items; memory: %s\n", items.n, manual ? "manual (#NOT_GARBAGE)" : "garbage collector");
    if (diag_error_count()) {
        fprintf(stderr, "\n%d syntax error(s). Compilation aborted.\n", diag_error_count());
        return 1;
    }

    /* 4) semantics */
    Program *prog = sema_analyze(rt_items, items);
    if (diag_error_count()) {
        fprintf(stderr, "\n%d error(s)%s. Compilation aborted.\n", diag_error_count(),
                diag_warning_count() ? xsprintf(" and %d warning(s)", diag_warning_count()) : "");
        return 1;
    }
    if (check) {
        fprintf(stderr, "%s: ok (%d warning(s)) in %.2f ms\n", input, diag_warning_count(), now_ms() - t_start);
        return 0;
    }

    /* 5) machine code + PE */
    char *out = output ? xstrdup(output) : default_output(input);
    codegen_emit(prog, out);
    if (diag_error_count()) return 1;
    {
        FILE *f = fopen(out, "rb");
        long sz = 0;
        if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
        fprintf(stderr, "compiled: %s (%ld bytes, %s memory) in %.2f ms\n", out, sz, prog->no_gc ? "manual" : "garbage-collected", now_ms() - t_start);
    }
    return 0;
}
