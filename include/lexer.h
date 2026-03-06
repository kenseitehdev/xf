#ifndef XF_LEX_H
#define XF_LEX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ============================================================
 * xf lexer
 *
 * Usage:
 *   Lexer lex;
 *   xf_lex_init(&lex, src, src_len, XF_SRC_INLINE, "<test>");
 *   xf_tokenize(&lex);          // fills lex.tokens[0..count-1]
 *   Token *t = &lex.tokens[i];  // parser walks by index
 *   xf_lex_free(&lex);
 *
 * next() is the internal scan function — produces one Token
 * and appends it to lex->tokens. Callers use xf_tokenize() to
 * drain to EOF, or xf_lex_step() for one token at a time (REPL).
 * ============================================================ */


/* ------------------------------------------------------------
 * Token kinds
 * ------------------------------------------------------------ */

typedef enum {
    /* ── literals ─────────────────────────────────────────── */
    TK_NUM,           /* 42  3.14  1e10  0xff                   */
    TK_STR,           /* "hello"  'world'                       */
    TK_REGEX,         /* /pattern/flags                         */
    TK_SUBST,         /* s/pattern/replacement/flags            */
    TK_TRANS,         /* y/from/to/   tr/from/to/               */
    TK_IDENT,         /* foo  bar_baz                           */

    /* ── type keywords ─────────────────────────────────────── */
    TK_KW_NUM,        /* num                                    */
    TK_KW_STR,        /* str                                    */
    TK_KW_MAP,        /* map                                    */
    TK_KW_SET,        /* set                                    */
    TK_KW_ARR,        /* arr                                    */
    TK_KW_FN,         /* fn                                     */
    TK_KW_VOID,       /* void  (type)                           */

    /* ── state keywords ────────────────────────────────────── */
    TK_KW_OK,         /* OK                                     */
    TK_KW_ERR,        /* ERR                                    */
    TK_KW_NAV,        /* NAV                                    */
    TK_KW_NULL,       /* NULL                                   */
    TK_KW_VOID_S,     /* VOID  (state, distinct from void type) */
    TK_KW_UNDEF,      /* UNDEF                                  */
    TK_KW_UNDET,      /* UNDETERMINED                           */

    /* ── control keywords ──────────────────────────────────── */
    TK_KW_BEGIN,      /* BEGIN                                  */
    TK_KW_END,        /* END                                    */
    TK_KW_IF,         /* if                                     */
    TK_KW_ELSE,       /* else                                   */
    TK_KW_ELIF,       /* elif                                   */
    TK_KW_WHILE,      /* while                                  */
    TK_KW_FOR,        /* for                                    */
    TK_KW_IN,         /* in                                     */
    TK_KW_RETURN,     /* return                                 */
    TK_KW_PRINT,      /* print                                  */
    TK_KW_PRINTF,     /* printf                                 */
    TK_KW_OUTFMT,     /* outfmt  "csv"|"tsv"|"json"|"text"     */
    TK_KW_SPAWN,      /* spawn                                  */
    TK_KW_JOIN,       /* join                                   */
    TK_KW_NEXT,       /* next                                   */
    TK_KW_EXIT,       /* exit                                   */
    TK_KW_DELETE,     /* delete                                 */
    TK_KW_IMPORT,     /* import                                 */

    /* ── implicit variables ─────────────────────────────────── */
    TK_FIELD,         /* $0 $1 $2 ... $n                        */
    TK_VAR_FILE,      /* $file                                  */
    TK_VAR_MATCH,     /* $match                                 */
    TK_VAR_CAPS,      /* $captures                              */
    TK_VAR_ERR,       /* $err                                   */
    TK_VAR_NR,        /* NR   record number (global)            */
    TK_VAR_NF,        /* NF   field count                       */
    TK_VAR_FNR,       /* FNR  record number (per file)          */
    TK_VAR_FS,        /* FS   field separator                   */
    TK_VAR_RS,        /* RS   record separator                  */
    TK_VAR_OFS,       /* OFS  output field separator            */
    TK_VAR_ORS,       /* ORS  output record separator           */

    /* ── arithmetic operators ──────────────────────────────── */
    TK_PLUS,          /* +                                      */
    TK_MINUS,         /* -                                      */
    TK_STAR,          /* *                                      */
    TK_SLASH,         /* /   context-sensitive: div vs regex    */
    TK_PERCENT,       /* %                                      */
    TK_CARET,         /* ^   exponentiation                     */
    TK_PLUS_EQ,       /* +=                                     */
    TK_MINUS_EQ,      /* -=                                     */
    TK_STAR_EQ,       /* *=                                     */
    TK_SLASH_EQ,      /* /=                                     */
    TK_PERCENT_EQ,    /* %=                                     */
    TK_PLUS_PLUS,     /* ++                                     */
    TK_MINUS_MINUS,   /* --                                     */

    /* ── comparison operators ──────────────────────────────── */
    TK_EQ_EQ,         /* ==                                     */
    TK_BANG_EQ,       /* !=                                     */
    TK_LT,            /* <                                      */
    TK_GT,            /* >   also: shorthand for  set[i] > body */
    TK_LT_EQ,         /* <=                                     */
    TK_GT_EQ,         /* >=                                     */
    TK_TILDE,         /* ~   regex match                        */
    TK_BANG_TILDE,    /* !~  regex no-match                     */
    TK_SPACESHIP,     /* <=> three-way compare                  */
    TK_DIAMOND,       /* <>  shorthand while: cond <> body      */

    /* ── logical operators ─────────────────────────────────── */
    TK_AMP_AMP,       /* &&                                     */
    TK_PIPE_PIPE,     /* ||                                     */
    TK_BANG,          /* !                                      */

    /* ── state / type introspection ────────────────────────── */
    TK_DOT_STATE,     /* .state  (val.state == OK)              */
    TK_DOT_TYPE,      /* .type   (val.type  == num)             */
    TK_DOT_LEN,       /* .len / .length  (str/arr/map/set)      */
    TK_DOT_PLUS,      /* .+  element-wise add (matrix)           */
    TK_DOT_MINUS,     /* .-  element-wise sub (matrix)           */
    TK_DOT_STAR,      /* .*  true matrix multiply                */
    TK_DOT_SLASH,     /* ./  element-wise divide (matrix)        */

    /* ── ternary / coalescing ───────────────────────────────── */
    TK_QUESTION,      /* ?   ternary: cond ? then : else        */
    TK_COALESCE,      /* ??  null/NAV coalescing                */

    /* ── string operators ──────────────────────────────────── */
    TK_DOT_DOT,       /* ..  string concat                      */
    TK_DOT_DOT_EQ,    /* ..= concat assign                      */

    /* ── pipe / redirect ───────────────────────────────────── */
    TK_PIPE,          /* |   pipe to command                    */
    TK_PIPE_GT,       /* |>  pipe to fn                         */
    TK_LT_LT,         /* <<  heredoc / input redirect           */
    TK_GT_GT,         /* >>  append redirect                    */

    /* ── assignment ────────────────────────────────────────── */
    TK_EQ,            /* =                                      */
    TK_WALRUS,        /* :=  declare + assign                   */

    /* ── delimiters ────────────────────────────────────────── */
    TK_LBRACE,        /* {                                      */
    TK_RBRACE,        /* }                                      */
    TK_LPAREN,        /* (                                      */
    TK_RPAREN,        /* )                                      */
    TK_LBRACKET,      /* [                                      */
    TK_RBRACKET,      /* ]                                      */
    TK_COMMA,         /* ,                                      */
    TK_SEMICOLON,     /* ;                                      */
    TK_COLON,         /* :                                      */
    TK_DOT,           /* .  member access                       */
    TK_DOTDOTDOT,     /* ... variadic / spread                  */

    /* ── repl commands ─────────────────────────────────────── */
    TK_REPL_CMD,      /* :help :state :type :load etc.          */

    /* ── structural ────────────────────────────────────────── */
    TK_NEWLINE,       /* significant newline in REPL mode       */
    TK_EOF,
    TK_ERROR,

} TokenKind;


/* ------------------------------------------------------------
 * Source location
 * ------------------------------------------------------------ */

typedef struct {
    const char *source;   /* filename, "<repl>", or "<inline>"  */
    uint32_t    line;
    uint32_t    col;
    uint32_t    offset;   /* byte offset from start of source   */
} Loc;


/* ------------------------------------------------------------
 * Token
 *
 * Lexemes for most tokens point into the source buffer (zero-copy).
 * Strings and regex are heap-allocated (escape processing) and owned
 * by the token — call xf_token_free() before discarding.
 * ------------------------------------------------------------ */

typedef struct {
    TokenKind   kind;
    Loc         loc;

    const char *lexeme;       /* raw source slice or owned copy */
    size_t      lexeme_len;
    bool        lexeme_owned; /* if true, free lexeme on cleanup */

    union {
        double   num;               /* TK_NUM                       */
        struct {
            const char *data;
            size_t      len;
        } str;                      /* TK_STR                       */
        struct {
            const char *pattern;
            size_t      pattern_len;
            const char *replacement;  /* TK_SUBST only              */
            size_t      replacement_len;
            uint32_t    flags;        /* XF_RE_* from xf_value.h    */
        } re;                       /* TK_REGEX / TK_SUBST          */
        struct {
            const char *from;
            size_t      from_len;
            const char *to;
            size_t      to_len;
        } trans;                    /* TK_TRANS                     */
        int      field_idx;         /* TK_FIELD: $0=0, $1=1 ...    */
        char     repl_cmd[64];      /* TK_REPL_CMD                  */
    } val;

} Token;


/* ------------------------------------------------------------
 * Lexer source modes
 * ------------------------------------------------------------ */

typedef enum {
    XF_SRC_FILE,    /* .xf script file                  */
    XF_SRC_INLINE,  /* -e 'expression'                  */
    XF_SRC_REPL,    /* interactive REPL — line-by-line  */
} SrcMode;


/* ------------------------------------------------------------
 * Lexer
 *
 * After xf_tokenize(), tokens[0..count-1] holds the full stream
 * including the terminal TK_EOF. The parser indexes into tokens[]
 * directly — no pull-based iteration needed.
 * ------------------------------------------------------------ */

#define XF_TOKENS_INIT_CAP 64

typedef struct {
    /* source */
    const char *src;
    size_t      src_len;
    size_t      pos;
    SrcMode     mode;
    const char *source_name;

    /* location tracking */
    uint32_t    line;
    uint32_t    col;

    /* context flags */
    bool        after_value;    /* disambiguates / (div vs regex) */
    bool        at_line_start;

    /* depth counters for REPL continuation detection */
    int         brace_depth;
    int         paren_depth;
    int         bracket_depth;

    /* token array — the primary output */
    Token      *tokens;         /* heap-allocated, grown as needed */
    size_t      count;          /* tokens produced so far          */
    size_t      capacity;       /* allocated slots                 */

    /* error state */
    bool        had_error;
    char        err_msg[256];

} Lexer;


/* ------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------ */

/* initialise from a buffer */
void   xf_lex_init(Lexer *lex, const char *src, size_t src_len,
                   SrcMode mode, const char *source_name);

/* initialise from a null-terminated string */
void   xf_lex_init_cstr(Lexer *lex, const char *src,
                        SrcMode mode, const char *source_name);

/* free the token array and any owned lexeme strings */
void   xf_lex_free(Lexer *lex);

/*
 * xf_tokenize — call next() repeatedly until TK_EOF,
 * filling lex->tokens[]. Returns total token count.
 * Primary entry point for file / inline modes.
 */
size_t xf_tokenize(Lexer *lex);

/*
 * xf_lex_step — call next() once and return a pointer to the
 * newly appended token. Used in REPL mode to process one token
 * at a time between user input lines.
 */
Token *xf_lex_step(Lexer *lex);

/* true if inside unbalanced {}, (), [] */
bool   xf_lex_is_continuation(const Lexer *lex);

/* free heap memory owned by a single token */
void   xf_token_free(Token *tok);

/* human-readable token kind name */
const char *xf_token_kind_name(TokenKind kind);

/* print token to stdout */
void   xf_token_print(const Token *tok);


/* ------------------------------------------------------------
 * Keyword lookup
 * ------------------------------------------------------------ */

typedef struct {
    const char *word;
    TokenKind   kind;
} Keyword;

TokenKind xf_keyword_lookup(const char *word, size_t len);
TokenKind xf_implicit_var_lookup(const char *word, size_t len);

#endif /* XF_LEX_H */
