#include "../include/lexer.h"
#include "../include/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================
 * Keyword tables
 * ============================================================ */

/* ============================================================
 * Keyword tables — kept in strict ASCII-lexicographic order
 * so that xf_keyword_lookup can use bsearch() for O(log n)
 * lookup instead of the naive O(n) linear scan.
 * ============================================================ */

static const Keyword keywords[] = {
    { "BEGIN",   TK_KW_BEGIN  },
    { "END",     TK_KW_END    },
    { "ERR",     TK_KW_ERR    },
    { "FNR",     TK_VAR_FNR   },
    { "FS",      TK_VAR_FS    },
    { "NAV",     TK_KW_NAV    },
    { "NF",      TK_VAR_NF    },
    { "NR",      TK_VAR_NR    },
    { "NULL",    TK_KW_NULL   },
    { "OFS",     TK_VAR_OFS   },
    { "OFMT",    TK_VAR_OFMT  },
    { "OK",      TK_KW_OK     },
    { "ORS",     TK_VAR_ORS   },
    { "RS",      TK_VAR_RS    },
    { "UNDEF",   TK_KW_UNDEF  },
    { "VOID",    TK_KW_VOID_S },
    { "arr",     TK_KW_ARR    },
    { "break",   TK_KW_BREAK  },
    { "delete",  TK_KW_DELETE },
    { "elif",    TK_KW_ELIF   },
    { "else",    TK_KW_ELSE   },
    { "exit",    TK_KW_EXIT   },
    { "fn",      TK_KW_FN     },
    { "for",     TK_KW_FOR    },
    { "if",      TK_KW_IF     },
    { "import",  TK_KW_IMPORT },
    { "in",      TK_KW_IN     },
    { "join",    TK_KW_JOIN   },
    { "map",     TK_KW_MAP    },
    { "next",    TK_KW_NEXT   },
    { "num",     TK_KW_NUM    },
    { "outfmt",  TK_KW_OUTFMT },
    { "print",   TK_KW_PRINT  },
    { "printf",  TK_KW_PRINTF },
    { "return",  TK_KW_RETURN },
    { "set",     TK_KW_SET    },
    { "spawn",   TK_KW_SPAWN  },
    { "str",     TK_KW_STR    },
    { "tuple",   TK_KW_TUPLE  },
    { "void",    TK_KW_VOID   },
    { "while",   TK_KW_WHILE  },
};
#define KEYWORD_COUNT (sizeof(keywords) / sizeof(keywords[0]))

static const Keyword implicit_vars[] = {
    { "captures", TK_VAR_CAPS  },
    { "err",      TK_VAR_ERR   },
    { "file",     TK_VAR_FILE  },
    { "match",    TK_VAR_MATCH },
};
#define IMPLICIT_VAR_COUNT (sizeof(implicit_vars) / sizeof(implicit_vars[0]))

/* bsearch comparator: compares a NUL-terminated needle (key)
 * against a Keyword entry, lexicographically. */
static int kw_cmp(const void *key, const void *elem) {
    return strcmp((const char *)key, ((const Keyword *)elem)->word);
}

/* O(log n) keyword lookup using the sorted tables above.
 * We NUL-terminate a local copy of `word` (max 32 chars)
 * so we can pass a cstr to bsearch without a malloc. */
TokenKind xf_keyword_lookup(const char *word, size_t len) {
    if (len == 0 || len > 31) return TK_IDENT;
    char buf[32];
    memcpy(buf, word, len);
    buf[len] = '\0';
    const Keyword *kw = bsearch(buf, keywords, KEYWORD_COUNT,
                                sizeof(Keyword), kw_cmp);
    return kw ? kw->kind : TK_IDENT;
}

TokenKind xf_implicit_var_lookup(const char *word, size_t len) {
    if (len == 0 || len > 31) return TK_IDENT;
    char buf[32];
    memcpy(buf, word, len);
    buf[len] = '\0';
    const Keyword *kw = bsearch(buf, implicit_vars, IMPLICIT_VAR_COUNT,
                                sizeof(Keyword), kw_cmp);
    return kw ? kw->kind : TK_IDENT;
}


/* ============================================================
 * Init / free
 * ============================================================ */

void xf_lex_init(Lexer *lex, const char *src, size_t src_len,
                 SrcMode mode, const char *source_name) {
    memset(lex, 0, sizeof(*lex));
    lex->src         = src;
    lex->src_len     = src_len;
    lex->mode        = mode;
    lex->source_name = source_name ? source_name : "<unknown>";
    lex->line        = 1;
    lex->col         = 1;
    lex->at_line_start = true;
    lex->capacity    = XF_TOKENS_INIT_CAP;
    lex->tokens      = malloc(sizeof(Token) * lex->capacity);
    lex->count       = 0;
}

void xf_lex_init_cstr(Lexer *lex, const char *src,
                      SrcMode mode, const char *source_name) {
    xf_lex_init(lex, src, strlen(src), mode, source_name);
}

void xf_lex_free(Lexer *lex) {
    if (!lex->tokens) return;
    for (size_t i = 0; i < lex->count; i++)
        xf_token_free(&lex->tokens[i]);
    free(lex->tokens);
    lex->tokens   = NULL;
    lex->count    = 0;
    lex->capacity = 0;
}

void xf_token_free(Token *tok) {
    if (tok->lexeme_owned) {
        free((char *)tok->lexeme);
        tok->lexeme       = NULL;
        tok->lexeme_owned = false;
    }
}


/* ============================================================
 * Internal helpers
 * ============================================================ */

static inline bool at_end(const Lexer *l)      { return l->pos >= l->src_len; }
static inline char peek(const Lexer *l)         { return at_end(l) ? '\0' : l->src[l->pos]; }
static inline char peek2(const Lexer *l)        { return (l->pos+1 < l->src_len) ? l->src[l->pos+1] : '\0'; }

static inline char advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; l->at_line_start = true; }
    else           { l->col++;              l->at_line_start = false; }
    return c;
}

static inline bool match(Lexer *l, char expected) {
    if (at_end(l) || l->src[l->pos] != expected) return false;
    advance(l);
    return true;
}

static inline Loc cur_loc(const Lexer *l) {
    return (Loc){ .source = l->source_name, .line = l->line,
                  .col = l->col, .offset = (uint32_t)l->pos };
}

static Token make_tok(Lexer *l, TokenKind kind, size_t start, Loc loc) {
    Token t = {0};
    t.kind       = kind;
    t.loc        = loc;
    t.lexeme     = l->src + start;
    t.lexeme_len = l->pos - start;
    return t;
}

static Token error_tok(Lexer *l, const char *msg) {
    l->had_error = true;
    snprintf(l->err_msg, sizeof(l->err_msg), "%s", msg);
    Token t = {0};
    t.kind       = TK_ERROR;
    t.loc        = cur_loc(l);
    t.lexeme     = l->err_msg;
    t.lexeme_len = strlen(msg);
    return t;
}

/* grow token array and append */
static Token *append_token(Lexer *l, Token t) {
    if (l->count >= l->capacity) {
        l->capacity *= 2;
        l->tokens = realloc(l->tokens, sizeof(Token) * l->capacity);
    }
    l->tokens[l->count] = t;
    return &l->tokens[l->count++];
}


/* ============================================================
 * Whitespace / comment skip
 * ============================================================ */

static void skip_ws(Lexer *l) {
    for (;;) {
        switch (peek(l)) {
            case ' ': case '\t': case '\r':
                advance(l);
                break;
            case '\n':
                if (l->mode == XF_SRC_REPL &&
                    l->brace_depth == 0 &&
                    l->paren_depth == 0 &&
                    l->bracket_depth == 0)
                    return;           /* significant newline — let next() emit it */
                advance(l);
                break;
            case '#':
                while (!at_end(l) && peek(l) != '\n') advance(l);
                break;
            default:
                return;
        }
    }
}


/* ============================================================
 * Sub-scanners
 * ============================================================ */

static Token scan_number(Lexer *l, size_t start, Loc loc) {
    bool is_hex = false;
    if (l->src[start] == '0' && (peek(l) == 'x' || peek(l) == 'X')) {
        advance(l);
        while (isxdigit((unsigned char)peek(l))) advance(l);
        is_hex = true;
    } else {
        while (isdigit((unsigned char)peek(l))) advance(l);
        if (peek(l) == '.' && isdigit((unsigned char)peek2(l))) {
            advance(l);
            while (isdigit((unsigned char)peek(l))) advance(l);
        }
        if (peek(l) == 'e' || peek(l) == 'E') {
            advance(l);
            if (peek(l) == '+' || peek(l) == '-') advance(l);
            while (isdigit((unsigned char)peek(l))) advance(l);
        }
    }
    Token t = make_tok(l, TK_NUM, start, loc);
    char buf[64];
    size_t len = t.lexeme_len < 63 ? t.lexeme_len : 63;
    memcpy(buf, t.lexeme, len); buf[len] = '\0';
    t.val.num = is_hex ? (double)strtoll(buf, NULL, 16) : strtod(buf, NULL);
    l->after_value = true;
    return t;
}

static Token scan_string(Lexer *l, char quote, Loc loc) {
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    while (!at_end(l)) {
        char c = peek(l);
        if (c == quote)  { advance(l); break; }
        if (c == '\n')   { free(buf); return error_tok(l, "unterminated string"); }
        if (c == '\\') {
            advance(l);
            char esc = advance(l);
            if (len+1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            switch (esc) {
                case 'n': buf[len++] = '\n'; break;
                case 't': buf[len++] = '\t'; break;
                case 'r': buf[len++] = '\r'; break;
                case '0': buf[len++] = '\0'; break;
                default:  buf[len++] = esc;  break;
            }
        } else {
            if (len+1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = advance(l);
        }
    }
    buf[len] = '\0';
    Token t = {0};
    t.kind            = TK_STR;
    t.loc             = loc;
    t.lexeme          = buf;
    t.lexeme_len      = len;
    t.lexeme_owned    = true;
    t.val.str.data    = buf;
    t.val.str.len     = len;
    l->after_value = true;
    return t;
}

static char *read_until(Lexer *l, char delim, size_t *out_len) {
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    while (!at_end(l)) {
        char c = peek(l);
        if (c == delim) { advance(l); break; }
        if (c == '\\' && peek2(l) == delim) advance(l);
        if (len+1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = advance(l);
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static uint32_t read_re_flags(Lexer *l) {
    uint32_t f = 0;
    while (!at_end(l)) {
        char c = peek(l);
        if      (c == 'g') { f |= XF_RE_GLOBAL;    advance(l); }
        else if (c == 'i') { f |= XF_RE_ICASE;     advance(l); }
        else if (c == 'm') { f |= XF_RE_MULTILINE;  advance(l); }
        else if (c == 'x') { f |= XF_RE_EXTENDED;  advance(l); }
        else break;
    }
    return f;
}

static Token scan_regex(Lexer *l, Loc loc) {
    /* opening / already consumed */
    size_t pat_len;
    char *pat = read_until(l, '/', &pat_len);
    if (!pat) return error_tok(l, "unterminated regex");
    uint32_t flags = read_re_flags(l);
    Token t = {0};
    t.kind               = TK_REGEX;
    t.loc                = loc;
    t.lexeme             = pat;
    t.lexeme_len         = pat_len;
    t.lexeme_owned       = true;
    t.val.re.pattern     = pat;
    t.val.re.pattern_len = pat_len;
    t.val.re.flags       = flags;
    l->after_value = true;
    return t;
}

static Token scan_subst(Lexer *l, Loc loc) {
    char delim = advance(l);
    size_t pat_len, rep_len;
    char *pat = read_until(l, delim, &pat_len);
    char *rep = read_until(l, delim, &rep_len);
    uint32_t flags = read_re_flags(l);
    Token t = {0};
    t.kind                   = TK_SUBST;
    t.loc                    = loc;
    t.lexeme                 = pat;
    t.lexeme_len             = pat_len;
    t.lexeme_owned           = true;
    t.val.re.pattern         = pat;
    t.val.re.pattern_len     = pat_len;
    t.val.re.replacement     = rep;
    t.val.re.replacement_len = rep_len;
    t.val.re.flags           = flags;
    l->after_value = false;
    return t;
}

static Token scan_trans(Lexer *l, Loc loc) {
    char delim = advance(l);
    size_t from_len, to_len;
    char *from = read_until(l, delim, &from_len);
    char *to   = read_until(l, delim, &to_len);
    Token t = {0};
    t.kind               = TK_TRANS;
    t.loc                = loc;
    t.lexeme             = from;
    t.lexeme_len         = from_len;
    t.lexeme_owned       = true;
    t.val.trans.from     = from;
    t.val.trans.from_len = from_len;
    t.val.trans.to       = to;
    t.val.trans.to_len   = to_len;
    l->after_value = false;
    return t;
}

static Token scan_dollar(Lexer *l, Loc loc) {
    size_t start = l->pos - 1;
    if (isdigit((unsigned char)peek(l))) {
        int idx = 0;
        while (isdigit((unsigned char)peek(l))) idx = idx*10 + (advance(l)-'0');
        Token t = make_tok(l, TK_FIELD, start, loc);
        t.val.field_idx = idx;
        l->after_value = true;
        return t;
    }
    if (isalpha((unsigned char)peek(l)) || peek(l) == '_') {
        size_t name_start = l->pos;
        while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
        size_t name_len = l->pos - name_start;
        TokenKind kind = xf_implicit_var_lookup(l->src + name_start, name_len);
        Token t = make_tok(l, kind, start, loc);
        l->after_value = true;
        return t;
    }
    return error_tok(l, "expected field index or variable name after '$'");
}

static Token scan_ident(Lexer *l, size_t start, Loc loc) {
    while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
    size_t len = l->pos - start;
    const char *word = l->src + start;

    /* shorthand s/// y/// tr/// — only when immediately followed by delimiter '/' */
    if (len == 1 && word[0] == 's' && peek(l) == '/')
        return scan_subst(l, loc);
    if (len == 1 && word[0] == 'y' && peek(l) == '/')
        return scan_trans(l, loc);
    if (len == 2 && word[0] == 't' && word[1] == 'r' && peek(l) == '/')
        return scan_trans(l, loc);

    TokenKind kind = xf_keyword_lookup(word, len);
    Token t = make_tok(l, kind, start, loc);
    l->after_value = (kind == TK_IDENT     || kind == TK_KW_OK  ||
                      kind == TK_KW_ERR    || kind == TK_KW_NAV ||
                      kind == TK_KW_NULL   || kind == TK_KW_VOID_S ||
                      kind == TK_KW_UNDEF);
    return t;
}

static Token scan_repl_cmd(Lexer *l, Loc loc) {
    size_t start = l->pos;   /* after : */
    while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
    Token t = {0};
    t.kind       = TK_REPL_CMD;
    t.loc        = loc;
    t.lexeme     = l->src + start - 1;   /* include the : */
    t.lexeme_len = l->pos - start + 1;
    size_t cmd_len = l->pos - start;
    if (cmd_len >= sizeof(t.val.repl_cmd)) cmd_len = sizeof(t.val.repl_cmd)-1;
    memcpy(t.val.repl_cmd, l->src + start, cmd_len);
    t.val.repl_cmd[cmd_len] = '\0';
    return t;
}


/* ============================================================
 * next() — scan one token, append to lex->tokens, return ptr
 *
 * This is the core of the lexer. All public entry points
 * (xf_tokenize, xf_lex_step) call next() to produce tokens.
 * ============================================================ */

static Token *next(Lexer *l) {
    skip_ws(l);

    if (at_end(l))
        return append_token(l, make_tok(l, TK_EOF, l->pos, cur_loc(l)));

    /* significant newline in REPL mode at statement boundary */
    if (l->mode == XF_SRC_REPL && peek(l) == '\n' &&
        l->brace_depth == 0 && l->paren_depth == 0 && l->bracket_depth == 0) {
        Loc loc = cur_loc(l);
        size_t start = l->pos;
        advance(l);
        l->after_value = false;
        return append_token(l, make_tok(l, TK_NEWLINE, start, loc));
    }

    Loc    loc   = cur_loc(l);
    size_t start = l->pos;
    char   c     = advance(l);

    if (isdigit((unsigned char)c))
        return append_token(l, scan_number(l, start, loc));

    if (c == '"' || c == '\'')
        return append_token(l, scan_string(l, c, loc));

    if (isalpha((unsigned char)c) || c == '_')
        return append_token(l, scan_ident(l, start, loc));

    if (c == '$')
        return append_token(l, scan_dollar(l, loc));

    Token t = {0};
    switch (c) {

        case '/':
            if (!l->after_value) { t = scan_regex(l, loc); break; }
            if (match(l, '='))   { t = make_tok(l, TK_SLASH_EQ, start, loc); l->after_value = false; break; }
            t = make_tok(l, TK_SLASH, start, loc); l->after_value = false; break;

        case '+':
            if (match(l, '+')) { t = make_tok(l, TK_PLUS_PLUS,  start, loc); l->after_value = true;  break; }
            if (match(l, '=')) { t = make_tok(l, TK_PLUS_EQ,    start, loc); l->after_value = false; break; }
            t = make_tok(l, TK_PLUS, start, loc); l->after_value = false; break;

        case '-':
            if (match(l, '-')) { t = make_tok(l, TK_MINUS_MINUS, start, loc); l->after_value = true;  break; }
            if (match(l, '=')) { t = make_tok(l, TK_MINUS_EQ,    start, loc); l->after_value = false; break; }
            t = make_tok(l, TK_MINUS, start, loc); l->after_value = false; break;

        case '*':
            if (match(l, '=')) { t = make_tok(l, TK_STAR_EQ, start, loc); l->after_value = false; break; }
            t = make_tok(l, TK_STAR, start, loc); l->after_value = false; break;

        case '%':
            if (match(l, '=')) { t = make_tok(l, TK_PERCENT_EQ, start, loc); break; }
            t = make_tok(l, TK_PERCENT, start, loc); break;

        case '^': t = make_tok(l, TK_CARET, start, loc); break;

        case '=':
            if (match(l, '=')) { t = make_tok(l, TK_EQ_EQ, start, loc); l->after_value = false; break; }
            t = make_tok(l, TK_EQ, start, loc); l->after_value = false; break;

        case '!':
            if (match(l, '=')) { t = make_tok(l, TK_BANG_EQ,    start, loc); l->after_value = false; break; }
            if (match(l, '~')) { t = make_tok(l, TK_BANG_TILDE, start, loc); l->after_value = false; break; }
            t = make_tok(l, TK_BANG, start, loc); l->after_value = false; break;

        case '<':
            if (match(l, '=')) {
                if (match(l, '>')) { t = make_tok(l, TK_SPACESHIP, start, loc); break; } /* <=> */
                t = make_tok(l, TK_LT_EQ, start, loc); break;                            /* <=  */
            }
            if (match(l, '<')) { t = make_tok(l, TK_LT_LT,  start, loc); break; }       /* <<  */
            if (match(l, '>')) { t = make_tok(l, TK_DIAMOND, start, loc); break; }       /* <>  */
            t = make_tok(l, TK_LT, start, loc); break;

        case '>':
            if (match(l, '=')) { t = make_tok(l, TK_GT_EQ, start, loc); break; }
            if (match(l, '>')) { t = make_tok(l, TK_GT_GT, start, loc); break; }
            t = make_tok(l, TK_GT, start, loc); break;

        case '~':
            l->after_value = false;
            t = make_tok(l, TK_TILDE, start, loc); break;

        case '&':
            if (match(l, '&')) { t = make_tok(l, TK_AMP_AMP, start, loc); break; }
            t = error_tok(l, "unexpected '&', did you mean '&&'?"); break;

        case '|':
            if (match(l, '|')) { t = make_tok(l, TK_PIPE_PIPE, start, loc); break; }
            if (match(l, '>')) { t = make_tok(l, TK_PIPE_GT,   start, loc); break; }
            t = make_tok(l, TK_PIPE, start, loc); break;

        case '?':
            if (match(l, '?')) { t = make_tok(l, TK_COALESCE, start, loc); break; }
            t = make_tok(l, TK_QUESTION, start, loc); break;

        case '.':
            if (match(l, '.')) {
                if (match(l, '.')) { t = make_tok(l, TK_DOTDOTDOT, start, loc); break; }
                if (match(l, '=')) { t = make_tok(l, TK_DOT_DOT_EQ, start, loc); break; }
                t = make_tok(l, TK_DOT_DOT, start, loc); break;
            }
            if (l->pos + 5 <= l->src_len &&
                strncmp(l->src + l->pos, "state", 5) == 0 &&
                !isalnum((unsigned char)l->src[l->pos + 5])) {
                l->pos += 5; l->col += 5;
                t = make_tok(l, TK_DOT_STATE, start, loc); break;
            }
            if (l->pos + 4 <= l->src_len &&
                strncmp(l->src + l->pos, "type", 4) == 0 &&
                !isalnum((unsigned char)l->src[l->pos + 4])) {
                l->pos += 4; l->col += 4;
                t = make_tok(l, TK_DOT_TYPE, start, loc); break;
            }
            /* .len / .length */
            if (l->pos + 6 <= l->src_len &&
                strncmp(l->src + l->pos, "length", 6) == 0 &&
                !isalnum((unsigned char)l->src[l->pos + 6])) {
                l->pos += 6; l->col += 6;
                t = make_tok(l, TK_DOT_LEN, start, loc); break;
            }
            if (l->pos + 3 <= l->src_len &&
                strncmp(l->src + l->pos, "len", 3) == 0 &&
                !isalnum((unsigned char)l->src[l->pos + 3]) && l->src[l->pos+3] != '_') {
                l->pos += 3; l->col += 3;
                t = make_tok(l, TK_DOT_LEN, start, loc); break;
            }
            /* matrix ops: .+ .- .* ./ */
            if (match(l, '+')) { t = make_tok(l, TK_DOT_PLUS,  start, loc); break; }
            if (match(l, '-')) { t = make_tok(l, TK_DOT_MINUS, start, loc); break; }
            if (match(l, '*')) { t = make_tok(l, TK_DOT_STAR,  start, loc); break; }
            if (match(l, '/')) { t = make_tok(l, TK_DOT_SLASH, start, loc); break; }
            t = make_tok(l, TK_DOT, start, loc); break;

        case ':':
            if (l->mode == XF_SRC_REPL && isalpha((unsigned char)peek(l))) {
                t = scan_repl_cmd(l, loc); break;
            }
            if (match(l, '=')) { t = make_tok(l, TK_WALRUS, start, loc); break; }
            t = make_tok(l, TK_COLON, start, loc); break;

        case '{': l->brace_depth++;   l->after_value = false; t = make_tok(l, TK_LBRACE,   start, loc); break;
        case '}': if (l->brace_depth   > 0) l->brace_depth--;   l->after_value = true;  t = make_tok(l, TK_RBRACE,   start, loc); break;
        case '(': l->paren_depth++;   l->after_value = false; t = make_tok(l, TK_LPAREN,   start, loc); break;
        case ')': if (l->paren_depth   > 0) l->paren_depth--;   l->after_value = true;  t = make_tok(l, TK_RPAREN,   start, loc); break;
        case '[': l->bracket_depth++; l->after_value = false; t = make_tok(l, TK_LBRACKET, start, loc); break;
        case ']': if (l->bracket_depth > 0) l->bracket_depth--; l->after_value = true;  t = make_tok(l, TK_RBRACKET, start, loc); break;
        case ',': l->after_value = false; t = make_tok(l, TK_COMMA,     start, loc); break;
        case ';': l->after_value = false; t = make_tok(l, TK_SEMICOLON, start, loc); break;

        default: {
            char msg[64];
            snprintf(msg, sizeof(msg), "unexpected character '%c' (0x%02x)", c, (unsigned char)c);
            t = error_tok(l, msg);
        }
    }

    return append_token(l, t);
}


/* ============================================================
 * Public entry points
 * ============================================================ */

size_t xf_tokenize(Lexer *l) {
    for (;;) {
        Token *t = next(l);
        if (t->kind == TK_EOF || t->kind == TK_ERROR) break;
    }
    return l->count;
}

Token *xf_lex_step(Lexer *l) {
    return next(l);
}

bool xf_lex_is_continuation(const Lexer *l) {
    return l->brace_depth > 0 || l->paren_depth > 0 || l->bracket_depth > 0;
}


/* ============================================================
 * Display
 * ============================================================ */

const char *xf_token_kind_name(TokenKind kind) {
    switch (kind) {
        case TK_NUM:          return "NUM";
        case TK_STR:          return "STR";
        case TK_REGEX:        return "REGEX";
        case TK_SUBST:        return "SUBST";
        case TK_TRANS:        return "TRANS";
        case TK_IDENT:        return "IDENT";
        case TK_KW_NUM:       return "kw:num";
        case TK_KW_STR:       return "kw:str";
        case TK_KW_MAP:       return "kw:map";
        case TK_KW_SET:       return "kw:set";
        case TK_KW_ARR:       return "kw:arr";
        case TK_KW_FN:        return "kw:fn";
        case TK_KW_VOID:      return "kw:void";
        case TK_KW_OK:        return "kw:OK";
        case TK_KW_ERR:       return "kw:ERR";
        case TK_KW_NAV:       return "kw:NAV";
        case TK_KW_NULL:      return "kw:NULL";
        case TK_KW_VOID_S:    return "kw:VOID";
        case TK_KW_UNDEF:     return "kw:UNDEF";
        case TK_KW_BEGIN:     return "kw:BEGIN";
        case TK_KW_END:       return "kw:END";
        case TK_KW_IF:        return "kw:if";
        case TK_KW_ELSE:      return "kw:else";
        case TK_KW_ELIF:      return "kw:elif";
        case TK_KW_WHILE:     return "kw:while";
        case TK_KW_FOR:       return "kw:for";
        case TK_KW_IN:        return "kw:in";
        case TK_KW_RETURN:    return "kw:return";
        case TK_KW_PRINT:     return "kw:print";
        case TK_KW_PRINTF:    return "kw:printf";
        case TK_KW_OUTFMT:    return "kw:outfmt";
        case TK_KW_SPAWN:     return "kw:spawn";
        case TK_KW_JOIN:      return "kw:join";
        case TK_KW_NEXT:      return "kw:next";
        case TK_KW_EXIT:      return "kw:exit";
        case TK_KW_BREAK:     return "kw:break";
        case TK_KW_DELETE:    return "kw:delete";
        case TK_KW_IMPORT:    return "kw:import";
        case TK_FIELD:        return "FIELD";
        case TK_VAR_FILE:     return "$file";
        case TK_VAR_MATCH:    return "$match";
        case TK_VAR_CAPS:     return "$captures";
        case TK_VAR_ERR:      return "$err";
        case TK_VAR_NR:       return "NR";
        case TK_VAR_NF:       return "NF";
        case TK_VAR_FNR:      return "FNR";
        case TK_VAR_FS:       return "FS";
        case TK_VAR_RS:       return "RS";
        case TK_VAR_OFS:      return "OFS";
        case TK_VAR_ORS:      return "ORS";
        case TK_VAR_OFMT:     return "OFMT";
        case TK_PLUS:         return "+";
        case TK_MINUS:        return "-";
        case TK_STAR:         return "*";
        case TK_SLASH:        return "/";
        case TK_PERCENT:      return "%";
        case TK_CARET:        return "^";
        case TK_PLUS_EQ:      return "+=";
        case TK_MINUS_EQ:     return "-=";
        case TK_STAR_EQ:      return "*=";
        case TK_SLASH_EQ:     return "/=";
        case TK_PERCENT_EQ:   return "%=";
        case TK_PLUS_PLUS:    return "++";
        case TK_MINUS_MINUS:  return "--";
        case TK_EQ_EQ:        return "==";
        case TK_BANG_EQ:      return "!=";
        case TK_LT:           return "<";
        case TK_GT:           return ">";
        case TK_LT_EQ:        return "<=";
        case TK_GT_EQ:        return ">=";
        case TK_TILDE:        return "~";
        case TK_BANG_TILDE:   return "!~";
        case TK_SPACESHIP:    return "<=>";
        case TK_DIAMOND:      return "<>";
        case TK_AMP_AMP:      return "&&";
        case TK_PIPE_PIPE:    return "||";
        case TK_BANG:         return "!";
        case TK_DOT_STATE:    return ".state";
        case TK_DOT_PLUS:     return ".+";
        case TK_DOT_MINUS:    return ".-";
        case TK_DOT_STAR:     return ".*";
        case TK_DOT_SLASH:    return "./";
        case TK_DOT_LEN:      return ".len";
        case TK_DOT_TYPE:     return ".type";
        case TK_QUESTION:     return "?";
        case TK_COALESCE:     return "??";
        case TK_DOT_DOT:      return "..";
        case TK_DOT_DOT_EQ:   return "..=";
        case TK_PIPE:         return "|";
        case TK_PIPE_GT:      return "|>";
        case TK_LT_LT:        return "<<";
        case TK_GT_GT:        return ">>";
        case TK_EQ:           return "=";
        case TK_WALRUS:       return ":=";
        case TK_LBRACE:       return "{";
        case TK_RBRACE:       return "}";
        case TK_LPAREN:       return "(";
        case TK_RPAREN:       return ")";
        case TK_LBRACKET:     return "[";
        case TK_RBRACKET:     return "]";
        case TK_COMMA:        return ",";
        case TK_SEMICOLON:    return ";";
        case TK_COLON:        return ":";
        case TK_DOT:          return ".";
        case TK_DOTDOTDOT:    return "...";
        case TK_REPL_CMD:     return "REPL_CMD";
        case TK_NEWLINE:      return "NEWLINE";
        case TK_EOF:          return "EOF";
        case TK_ERROR:        return "ERROR";
        default:              return "?";
    }
}

void xf_token_print(const Token *t) {
    printf("[%s:%u:%u] %-16s",
           t->loc.source, t->loc.line, t->loc.col,
           xf_token_kind_name(t->kind));
    switch (t->kind) {
        case TK_NUM:      printf("  %g",          t->val.num);                                       break;
        case TK_STR:      printf("  \"%.*s\"",    (int)t->val.str.len,  t->val.str.data);            break;
        case TK_REGEX:    printf("  /%s/",         t->val.re.pattern);                               break;
        case TK_SUBST:    printf("  s/%s/%s/",     t->val.re.pattern, t->val.re.replacement);        break;
        case TK_TRANS:    printf("  y/%s/%s/",     t->val.trans.from,   t->val.trans.to);            break;
        case TK_FIELD:    printf("  $%d",           t->val.field_idx);                               break;
        case TK_IDENT:    printf("  %.*s",         (int)t->lexeme_len,   t->lexeme);                 break;
        case TK_REPL_CMD: printf("  :%s",           t->val.repl_cmd);                                break;
        default:
            if (t->lexeme_len) printf("  %.*s", (int)t->lexeme_len, t->lexeme);
            break;
    }
    printf("\n");
}