/*
 * lexer.c - Turns the source text into a list of tokens.
 *
 * Supports: // and block comments, strings with escapes, chars, integers
 * (decimal, 0x, 0b), floats, '#NAME' directives and all operators.
 */
#include "lexer.h"
#include <ctype.h>
#include <math.h>

typedef struct { const char *name; TokKind kind; } Keyword;
static const Keyword KEYWORDS[] = {
    {"if", KW_IF}, {"else", KW_ELSE}, {"while", KW_WHILE}, {"for", KW_FOR}, {"do", KW_DO},
    {"return", KW_RETURN}, {"break", KW_BREAK}, {"continue", KW_CONTINUE},
    {"class", KW_CLASS}, {"extends", KW_EXTENDS}, {"new", KW_NEW}, {"this", KW_THIS},
    {"super", KW_SUPER}, {"const", KW_CONST}, {"try", KW_TRY}, {"catch", KW_CATCH},
    {"finally", KW_FINALLY}, {"throw", KW_THROW}, {"true", KW_TRUE}, {"false", KW_FALSE},
    {"null", KW_NULL}, {"public", KW_PUBLIC}, {"private", KW_PRIVATE},
    {"protected", KW_PROTECTED}, {"package", KW_PACKAGE}, {"packege", KW_PACKAGE},
    {"sizeof", KW_SIZEOF}, {"switch", KW_SWITCH}, {"case", KW_CASE}, {"default", KW_DEFAULT},
    {"instanceof", KW_INSTANCEOF}, {"static", KW_STATIC}, {"abstract", KW_ABSTRACT}, {"interface", KW_INTERFACE},
    {"int", KW_INT}, {"long", KW_LONG}, {"float", KW_FLOAT}, {"double", KW_DOUBLE},
    {"bool", KW_BOOL}, {"boolean", KW_BOOL}, {"char", KW_CHAR}, {"String", KW_STRING},
    {"void", KW_VOID}, {"ptr", KW_PTR},
    {NULL, 0}
};

static const char *TOKEN_NAMES[TK__COUNT] = {
    [TK_EOF] = "end of file", [TK_IDENT] = "identifier", [TK_INT] = "integer number",
    [TK_FLOAT] = "real number", [TK_STRING] = "string", [TK_CHAR] = "character",
    [TK_DIRECTIVE] = "directive",
    [P_LPAREN] = "'('", [P_RPAREN] = "')'", [P_LBRACE] = "'{'", [P_RBRACE] = "'}'",
    [P_LBRACK] = "'['", [P_RBRACK] = "']'", [P_SEMI] = "';'", [P_COMMA] = "','",
    [P_DOT] = "'.'", [P_COLON] = "':'", [P_QUESTION] = "'?'",
    [P_PLUS] = "'+'", [P_MINUS] = "'-'", [P_STAR] = "'*'", [P_SLASH] = "'/'",
    [P_PERCENT] = "'%'", [P_AMP] = "'&'", [P_PIPE] = "'|'", [P_CARET] = "'^'",
    [P_TILDE] = "'~'", [P_BANG] = "'!'", [P_ASSIGN] = "'='", [P_EQ] = "'=='",
    [P_NE] = "'!='", [P_LT] = "'<'", [P_LE] = "'<='", [P_GT] = "'>'", [P_GE] = "'>='",
    [P_ANDAND] = "'&&'", [P_OROR] = "'||'", [P_SHL] = "'<<'", [P_SHR] = "'>>'",
    [P_PLUSPLUS] = "'++'", [P_MINUSMINUS] = "'--'", [P_PLUSEQ] = "'+='",
    [P_MINUSEQ] = "'-='", [P_STAREQ] = "'*='", [P_SLASHEQ] = "'/='",
    [P_PERCENTEQ] = "'%='", [P_AMPEQ] = "'&='", [P_PIPEEQ] = "'|='",
    [P_CARETEQ] = "'^='", [P_SHLEQ] = "'<<='", [P_SHREQ] = "'>>='",
};

const char *token_kind_name(TokKind k) {
    if (TOKEN_NAMES[k]) return TOKEN_NAMES[k];
    for (const Keyword *kw = KEYWORDS; kw->name; kw++)
        if (kw->kind == k) return xsprintf("'%s'", kw->name);
    return "?";
}

typedef struct {
    Source *src;
    const char *p;
    int line, col;
    Vec toks;   /* de Token* */
} Lexer;

static char peek(Lexer *lx, int off) { return lx->p[off]; }
static Loc here(Lexer *lx) { return (Loc){lx->src, lx->line, lx->col}; }

static char advance(Lexer *lx) {
    char c = *lx->p++;
    if (c == '\n') { lx->line++; lx->col = 1; }
    else if (c == '\t') lx->col++;
    else if (((uint8_t)c & 0xC0) != 0x80) lx->col++;   /* counts only the 1st UTF-8 byte */
    return c;
}

static Token *emit(Lexer *lx, TokKind k, Loc loc) {
    Token *t = xcalloc(sizeof(Token));
    t->kind = k;
    t->loc = loc;
    vec_push(&lx->toks, t);
    return t;
}

static bool is_ident_start(char c) { return isalpha((uint8_t)c) || c == '_' || ((uint8_t)c >= 0x80); }
static bool is_ident_char(char c) { return is_ident_start(c) || isdigit((uint8_t)c); }

/* reads an escape after the backslash; returns the byte */
static int read_escape(Lexer *lx) {
    Loc loc = here(lx);
    char c = advance(lx);
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return 0;
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case 'x': {
        int v = 0, n = 0;
        while (n < 2 && isxdigit((uint8_t)peek(lx, 0))) {
            char h = advance(lx);
            v = v * 16 + (isdigit((uint8_t)h) ? h - '0' : (tolower(h) - 'a' + 10));
            n++;
        }
        if (n == 0) diag_error(loc, "escape '\\x' has no hexadecimal digits");
        return v;
    }
    default:
        diag_error(loc, "unknown escape sequence '\\%c'", c);
        return c;
    }
}

static void lex_string(Lexer *lx) {
    Loc loc = here(lx);
    advance(lx);   /* " */
    int cap = 16, n = 0;
    char *buf = xcalloc((size_t)cap);
    while (peek(lx, 0) != '"') {
        if (peek(lx, 0) == '\0' || peek(lx, 0) == '\n') {
            diag_error(loc, "unterminated string");
            break;
        }
        int c = advance(lx);
        if (c == '\\') c = read_escape(lx);
        if (n + 2 > cap) { cap *= 2; buf = xrealloc(buf, (size_t)cap); }
        buf[n++] = (char)c;
    }
    if (peek(lx, 0) == '"') advance(lx);
    buf[n] = 0;
    Token *t = emit(lx, TK_STRING, loc);
    t->text = buf;
    t->len = n;
}

static void lex_char(Lexer *lx) {
    Loc loc = here(lx);
    advance(lx);   /* ' */
    int v = 0;
    if (peek(lx, 0) == '\\') { advance(lx); v = read_escape(lx); }
    else if (peek(lx, 0) == '\'' || peek(lx, 0) == '\0' || peek(lx, 0) == '\n') diag_error(loc, "empty character literal");
    else {
        /* decodes 1 UTF-8 codepoint (value > 255 becomes an error in the parser/sema) */
        uint8_t c = (uint8_t)advance(lx);
        if (c < 0x80) v = c;
        else {
            int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
            v = c & (0x3F >> extra);
            while (extra-- > 0 && ((uint8_t)peek(lx, 0) & 0xC0) == 0x80) v = (v << 6) | ((uint8_t)advance(lx) & 0x3F);
        }
    }
    if (peek(lx, 0) == '\'') advance(lx);
    else diag_error(loc, "unterminated character literal (expected ')");
    Token *t = emit(lx, TK_CHAR, loc);
    t->ival = v;
}

static void lex_number(Lexer *lx) {
    Loc loc = here(lx);
    const char *start = lx->p;
    if (peek(lx, 0) == '0' && (peek(lx, 1) == 'x' || peek(lx, 1) == 'X')) {
        advance(lx); advance(lx);
        uint64_t v = 0; int n = 0;
        while (isxdigit((uint8_t)peek(lx, 0))) {
            char h = advance(lx);
            v = v * 16 + (uint64_t)(isdigit((uint8_t)h) ? h - '0' : (tolower(h) - 'a' + 10));
            n++;
        }
        if (!n) diag_error(loc, "hexadecimal literal has no digits");
        emit(lx, TK_INT, loc)->ival = (int64_t)v;
        return;
    }
    if (peek(lx, 0) == '0' && (peek(lx, 1) == 'b' || peek(lx, 1) == 'B')) {
        advance(lx); advance(lx);
        uint64_t v = 0; int n = 0;
        while (peek(lx, 0) == '0' || peek(lx, 0) == '1') { v = v * 2 + (uint64_t)(advance(lx) - '0'); n++; }
        if (!n) diag_error(loc, "binary literal has no digits");
        emit(lx, TK_INT, loc)->ival = (int64_t)v;
        return;
    }
    bool is_float = false;
    while (isdigit((uint8_t)peek(lx, 0)) || peek(lx, 0) == '_') advance(lx);
    if (peek(lx, 0) == '.' && isdigit((uint8_t)peek(lx, 1))) {
        is_float = true;
        advance(lx);
        while (isdigit((uint8_t)peek(lx, 0)) || peek(lx, 0) == '_') advance(lx);
    }
    if ((peek(lx, 0) == 'e' || peek(lx, 0) == 'E') &&
        (isdigit((uint8_t)peek(lx, 1)) || ((peek(lx, 1) == '+' || peek(lx, 1) == '-') && isdigit((uint8_t)peek(lx, 2))))) {
        is_float = true;
        advance(lx); advance(lx);
        while (isdigit((uint8_t)peek(lx, 0))) advance(lx);
    }
    /* copy without '_' */
    char *txt = xcalloc((size_t)(lx->p - start) + 1);
    int n = 0;
    for (const char *q = start; q < lx->p; q++) if (*q != '_') txt[n++] = *q;
    if (is_float) {
        emit(lx, TK_FLOAT, loc)->fval = strtod(txt, NULL);
    } else {
        Token *t = emit(lx, TK_INT, loc);
        t->ival = (int64_t)strtoull(txt, NULL, 10);
        if (strlen(txt) > 19) diag_error(loc, "integer literal too large");
    }
}

static bool match(Lexer *lx, const char *s) {
    size_t n = strlen(s);
    if (strncmp(lx->p, s, n) == 0) {
        for (size_t i = 0; i < n; i++) advance(lx);
        return true;
    }
    return false;
}

static void lex_operator(Lexer *lx) {
    Loc loc = here(lx);
    static const struct { const char *s; TokKind k; } OPS[] = {
        {"<<=", P_SHLEQ}, {">>=", P_SHREQ},
        {"<<", P_SHL}, {">>", P_SHR}, {"++", P_PLUSPLUS}, {"--", P_MINUSMINUS},
        {"+=", P_PLUSEQ}, {"-=", P_MINUSEQ}, {"*=", P_STAREQ}, {"/=", P_SLASHEQ},
        {"%=", P_PERCENTEQ}, {"&=", P_AMPEQ}, {"|=", P_PIPEEQ}, {"^=", P_CARETEQ},
        {"==", P_EQ}, {"!=", P_NE}, {"<=", P_LE}, {">=", P_GE}, {"&&", P_ANDAND}, {"||", P_OROR},
        {"(", P_LPAREN}, {")", P_RPAREN}, {"{", P_LBRACE}, {"}", P_RBRACE},
        {"[", P_LBRACK}, {"]", P_RBRACK}, {";", P_SEMI}, {",", P_COMMA}, {".", P_DOT},
        {":", P_COLON}, {"?", P_QUESTION}, {"+", P_PLUS}, {"-", P_MINUS}, {"*", P_STAR},
        {"/", P_SLASH}, {"%", P_PERCENT}, {"&", P_AMP}, {"|", P_PIPE}, {"^", P_CARET},
        {"~", P_TILDE}, {"!", P_BANG}, {"=", P_ASSIGN}, {"<", P_LT}, {">", P_GT},
        {NULL, 0}
    };
    for (int i = 0; OPS[i].s; i++) {
        if (match(lx, OPS[i].s)) { emit(lx, OPS[i].k, loc); return; }
    }
    char c = advance(lx);
    if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) diag_error(loc, "unexpected character '%c'", c);
    else diag_error(loc, "unexpected character (code 0x%02x)", (uint8_t)c);
}

TokenList lex(Source *src) {
    Lexer lx = {.src = src, .p = src->text, .line = 1, .col = 1};
    for (;;) {
        char c = peek(&lx, 0);
        if (c == '\0') break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(&lx); continue; }
        if (c == '/' && peek(&lx, 1) == '/') {            /* line comment */
            while (peek(&lx, 0) && peek(&lx, 0) != '\n') advance(&lx);
            continue;
        }
        if (c == '/' && peek(&lx, 1) == '*') {            /* block comment */
            Loc loc = here(&lx);
            advance(&lx); advance(&lx);
            while (peek(&lx, 0) && !(peek(&lx, 0) == '*' && peek(&lx, 1) == '/')) advance(&lx);
            if (peek(&lx, 0)) { advance(&lx); advance(&lx); }
            else diag_error(loc, "unterminated block comment");
            continue;
        }
        if (c == '#') {                                    /* diretiva */
            Loc loc = here(&lx);
            advance(&lx);
            const char *s = lx.p;
            while (is_ident_char(peek(&lx, 0))) advance(&lx);
            Token *t = emit(&lx, TK_DIRECTIVE, loc);
            t->text = xstrndup(s, (size_t)(lx.p - s));
            t->len = (int)(lx.p - s);
            continue;
        }
        if (is_ident_start(c)) {
            Loc loc = here(&lx);
            const char *s = lx.p;
            while (is_ident_char(peek(&lx, 0))) advance(&lx);
            char *word = xstrndup(s, (size_t)(lx.p - s));
            TokKind k = TK_IDENT;
            for (const Keyword *kw = KEYWORDS; kw->name; kw++)
                if (strcmp(kw->name, word) == 0) { k = kw->kind; break; }
            Token *t = emit(&lx, k, loc);
            t->text = word;
            t->len = (int)(lx.p - s);
            continue;
        }
        if (isdigit((uint8_t)c)) { lex_number(&lx); continue; }
        if (c == '"') { lex_string(&lx); continue; }
        if (c == '\'') { lex_char(&lx); continue; }
        lex_operator(&lx);
    }
    emit(&lx, TK_EOF, here(&lx));
    TokenList out;
    out.n = lx.toks.n;
    out.t = xcalloc(sizeof(Token) * (size_t)out.n);
    for (int i = 0; i < out.n; i++) out.t[i] = *(Token *)lx.toks.v[i];
    return out;
}
