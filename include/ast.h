#ifndef XF_AST_H
#define XF_AST_H

#include "lexer.h"
#include "value.h"

/* ============================================================
 * xf AST
 *
 * Every node carries a Loc for error reporting.
 * Nodes are heap-allocated and owned by the tree.
 * ast_free(node) recursively frees a subtree.
 *
 * Node kinds split into two families:
 *   Stmt  — things that execute for effect
 *   Expr  — things that evaluate to a Value
 *
 * A program is a list of TopLevel items:
 *   - pattern-action rules
 *   - BEGIN / END blocks
 *   - function declarations
 *   - bare statements (inline / REPL)
 * ============================================================ */


/* ------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------ */

typedef struct Expr    Expr;
typedef struct Stmt    Stmt;
typedef struct Param   Param;
typedef struct Branch  Branch;


/* ============================================================
 * Expr — expression nodes
 * ============================================================ */

typedef enum {
    /* ── literals ─────────────────────────────────────────── */
    EXPR_NUM,         /* 42  3.14                               */
    EXPR_STR,         /* "hello"                                */
    EXPR_REGEX,       /* /pattern/flags                         */
    EXPR_ARR_LIT,
    EXPR_MAP_LIT,
    EXPR_SET_LIT,
/* ── variables ────────────────────────────────────────── */
    EXPR_IDENT,       /* foo                                    */
    EXPR_FIELD,       /* $0  $1  $n                             */
    EXPR_IVAR,        /* NR NF FNR FS RS OFS ORS                */
    EXPR_SVAR,        /* $file $match $captures $err            */

    /* ── operations ───────────────────────────────────────── */
    EXPR_UNARY,       /* -x  !x  ++x  x++                      */
    EXPR_BINARY,      /* x + y  x == y  x ~ /re/  etc.         */
    EXPR_TERNARY,     /* cond ? then : else                     */
    EXPR_COALESCE,    /* x ?? y  (null/NAV coalescing)          */

    /* ── assignment ───────────────────────────────────────── */
    EXPR_ASSIGN,      /* x = y   x += y  x ..= y  etc.         */
    EXPR_WALRUS,      /* x := y  (declare + assign)             */

    /* ── call / access ────────────────────────────────────── */
    EXPR_CALL,        /* f(a, b)                                */
    EXPR_SUBSCRIPT,   /* a[k]                                   */
    EXPR_MEMBER,      /* obj.field                              */

    /* ── state / type introspection ────────────────────────── */
    EXPR_STATE,       /* x.state                                */
    EXPR_TYPE,        /* x.type                                 */
    EXPR_LEN,         /* x.len / x.length  (str/arr/map/set)   */

    /* ── type cast ─────────────────────────────────────────── */
    EXPR_CAST,        /* num(x)  str(x)  arr(x)  map(x)  set(x) */

    /* ── pipeline ─────────────────────────────────────────── */
    EXPR_PIPE,        /* cmd | cmd                              */
    EXPR_PIPE_FN,     /* expr |> fn                             */

    /* ── spread / variadic ────────────────────────────────── */
    EXPR_SPREAD,      /* ...x                                   */

    /* ── anonymous function ───────────────────────────────── */
    EXPR_FN,          /* fn(params) { body }                    */

} ExprKind;


/* ── unary operator ─────────────────────────────────────── */
typedef enum {
    UNOP_NEG,         /* -x                                     */
    UNOP_NOT,         /* !x                                     */
    UNOP_PRE_INC,     /* ++x                                    */
    UNOP_PRE_DEC,     /* --x                                    */
    UNOP_POST_INC,    /* x++                                    */
    UNOP_POST_DEC,    /* x--                                    */
} UnOp;

/* ── binary operator ────────────────────────────────────── */
typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD, BINOP_POW,
    BINOP_MADD, BINOP_MSUB, BINOP_MMUL, BINOP_MDIV,
    BINOP_PIPE_CMD,  /* expr | "shell cmd" */
    BINOP_PIPE_IN,   /* "shell cmd" | expr  (read from cmd) */
    BINOP_EQ, BINOP_NEQ, BINOP_LT, BINOP_GT, BINOP_LTE, BINOP_GTE,
    BINOP_SPACESHIP,   /* <=>                                   */
    BINOP_AND, BINOP_OR,
    BINOP_MATCH,       /* ~   regex match                       */
    BINOP_NMATCH,      /* !~  regex no-match                    */
    BINOP_CONCAT,      /* ..  string concat                     */
    BINOP_PIPE,        /* |   command pipe                      */
} BinOp;

/* ── assignment operator ────────────────────────────────── */
typedef enum {
    ASSIGNOP_EQ,       /* =                                     */
    ASSIGNOP_ADD,      /* +=                                     */
    ASSIGNOP_SUB,      /* -=                                     */
    ASSIGNOP_MUL,      /* *=                                     */
    ASSIGNOP_DIV,      /* /=                                     */
    ASSIGNOP_MOD,      /* %=                                     */
    ASSIGNOP_CONCAT,   /* ..=                                    */
} AssignOp;


/* ── parameter (for fn literals) ────────────────────────── */
struct Param {
    xf_Str *name;
    uint8_t  type;        /* XF_TYPE_*                          */
    Expr    *default_val; /* NULL if no default                 */
    Loc      loc;
};


struct Expr {
    ExprKind kind;
    Loc      loc;
    uint8_t  type_hint;   /* inferred or annotated type         */

    union {
        /* EXPR_NUM */
        double num;

        /* EXPR_STR */
        struct { xf_Str *value; } str;

        /* EXPR_REGEX */
        struct {
            xf_Str  *pattern;
            uint32_t flags;    /* XF_RE_*                       */
        } regex;


        /* EXPR_ARR_LIT */
        struct {
            Expr   **items;
            size_t   count;
        } arr_lit;

        struct { Expr **keys; Expr **vals; size_t count; } map_lit;
        struct { Expr **items; size_t count; } set_lit;
        /* EXPR_IDENT */
        struct { xf_Str *name; } ident;

        /* EXPR_FIELD */
        struct { int index; } field;    /* $0=0, $1=1 ...       */

        /* EXPR_IVAR — implicit variable */
        struct { TokenKind var; } ivar; /* TK_VAR_NR etc.       */

        /* EXPR_SVAR — $ implicit variable */
        struct { TokenKind var; } svar; /* TK_VAR_FILE etc.     */

        /* EXPR_UNARY */
        struct {
            UnOp  op;
            Expr *operand;
        } unary;

        /* EXPR_BINARY */
        struct {
            BinOp  op;
            Expr  *left;
            Expr  *right;
        } binary;

        /* EXPR_TERNARY */
        struct {
            Expr *cond;
            Expr *then;
            Expr *els;
        } ternary;

        /* EXPR_COALESCE */
        struct {
            Expr *left;
            Expr *right;
        } coalesce;

        /* EXPR_ASSIGN */
        struct {
            AssignOp  op;
            Expr     *target;
            Expr     *value;
        } assign;

        /* EXPR_WALRUS */
        struct {
            xf_Str *name;
            uint8_t type;     /* declared type, XF_TYPE_VOID = inferred */
            Expr   *value;
        } walrus;

        /* EXPR_CALL */
        struct {
            Expr   *callee;
            Expr  **args;
            size_t  argc;
        } call;

        /* EXPR_SUBSCRIPT */
        struct {
            Expr *obj;
            Expr *key;
        } subscript;

        /* EXPR_MEMBER */
        struct {
            Expr    *obj;
            xf_Str  *field;
        } member;

        /* EXPR_STATE / EXPR_TYPE / EXPR_LEN — operand only */
        struct { Expr *operand; } introspect;

        /* EXPR_CAST — type(expr) */
        struct {
            uint8_t  to_type;   /* XF_TYPE_* target             */
            Expr    *operand;
        } cast;

        /* EXPR_PIPE_FN */
        struct {
            Expr *left;
            Expr *right;
        } pipe_fn;

        /* EXPR_SPREAD */
        struct { Expr *operand; } spread;

        /* EXPR_FN — anonymous function literal */
        struct {
            uint8_t  return_type;
            Param   *params;
            size_t   param_count;
            Stmt    *body;        /* STMT_BLOCK                 */
        } fn;

    } as;
};


/* ============================================================
 * Stmt — statement nodes
 * ============================================================ */

typedef enum {
    /* ── block ────────────────────────────────────────────── */
    STMT_BLOCK,       /* { stmt* }                              */

    /* ── expression statement ─────────────────────────────── */
    STMT_EXPR,        /* expr ;                                 */

    /* ── declaration ──────────────────────────────────────── */
    STMT_VAR_DECL,    /* type name = expr ;                     */
    STMT_FN_DECL,     /* type fn name(params) { body }          */

    /* ── control flow ─────────────────────────────────────── */
    STMT_IF,          /* if / elif / else                       */
    STMT_WHILE,       /* while (cond) { body }                  */
    STMT_FOR,         /* for (x in collection) { body }         */

    /* ── shorthand loops ──────────────────────────────────── */
    STMT_WHILE_SHORT, /* cond <> body ;                         */
    STMT_FOR_SHORT,   /* set[iter] > body ;                     */

    /* ── transfer ─────────────────────────────────────────── */
    STMT_RETURN,      /* return expr ;                          */
    STMT_NEXT,        /* next ;   (advance to next record)      */
    STMT_EXIT,        /* exit ;   (exit program)                */

    /* ── I/O ──────────────────────────────────────────────── */
    STMT_PRINT,       /* print expr, expr, ... ;                */
    STMT_PRINTF,      /* printf fmt, arg, ... ;                 */
    STMT_OUTFMT,      /* outfmt "csv"|"tsv"|"json"|"text" ;    */
    STMT_IMPORT,      /* import "file.xf" ;                     */
    STMT_DELETE,      /* delete map[key] ;                      */

    /* ── concurrency ──────────────────────────────────────── */
    STMT_SPAWN,       /* spawn fn(args) ;                       */
    STMT_JOIN,        /* join handle ;                          */

    /* ── substitution / transliteration ──────────────────── */
    STMT_SUBST,       /* s/pat/rep/flags                        */
    STMT_TRANS,       /* y/from/to/                             */

} StmtKind;


/* ── if/elif/else branch ────────────────────────────────── */
struct Branch {
    Expr   *cond;     /* NULL for final else                    */
    Stmt   *body;
    Loc     loc;
};


struct Stmt {
    StmtKind kind;
    Loc      loc;

    union {
        /* STMT_BLOCK */
        struct {
            Stmt  **stmts;
            size_t  count;
        } block;

        /* STMT_EXPR */
        struct { Expr *expr; } expr;

        /* STMT_VAR_DECL */
        struct {
            uint8_t  type;     /* XF_TYPE_*                    */
            xf_Str  *name;
            Expr    *init;     /* NULL = UNDETERMINED           */
        } var_decl;

        /* STMT_FN_DECL */
        struct {
            uint8_t  return_type;
            xf_Str  *name;
            Param   *params;
            size_t   param_count;
            Stmt    *body;
        } fn_decl;

        /* STMT_IF */
        struct {
            Branch *branches;  /* if + elif*                    */
            size_t  count;
            Stmt   *els;       /* else block, or NULL           */
        } if_stmt;

        /* STMT_WHILE */
        struct {
            Expr *cond;
            Stmt *body;
        } while_stmt;

        /* STMT_FOR */
        struct {
            xf_Str *iter;     /* loop variable name             */
            Expr   *collection;
            Stmt   *body;
        } for_stmt;

        /* STMT_WHILE_SHORT:  cond <> body */
        struct {
            Expr *cond;
            Stmt *body;
        } while_short;

        /* STMT_FOR_SHORT:  collection[iter] > body */
        struct {
            Expr   *collection;
            xf_Str *iter;
            Stmt   *body;
        } for_short;

        /* STMT_RETURN */
        struct { Expr *value; } ret;  /* NULL = void return     */

        /* STMT_NEXT / STMT_EXIT — no extra fields              */

        /* STMT_PRINT */
        struct {
            Expr  **args;
            size_t  count;
            Expr   *redirect;  /* > file, >> file, | cmd — NULL = stdout */
        } print;

        /* STMT_PRINTF — same shape; first arg is the format string */
        struct {
            Expr  **args;
            size_t  count;
            Expr   *redirect;
        } printf_stmt;

        /* STMT_OUTFMT */
        struct {
            uint8_t mode;   /* XF_OUTFMT_* constant             */
        } outfmt;

        /* STMT_IMPORT */
        struct { xf_Str *path; } import;

        /* STMT_DELETE */
        struct { Expr *target; } delete;

        /* STMT_SPAWN */
        struct { Expr *call; } spawn;   /* call must be EXPR_CALL */

        /* STMT_JOIN */
        struct { Expr *handle; } join;

        /* STMT_SUBST */
        struct {
            xf_Str  *pattern;
            xf_Str  *replacement;
            uint32_t flags;
            Expr    *target;   /* NULL = $0                     */
        } subst;

        /* STMT_TRANS */
        struct {
            xf_Str *from;
            xf_Str *to;
            Expr   *target;    /* NULL = $0                     */
        } trans;

    } as;
};


/* ============================================================
 * TopLevel — program-level items
 * ============================================================ */

typedef enum {
    TOP_BEGIN,        /* BEGIN { body }                         */
    TOP_END,          /* END   { body }                         */
    TOP_RULE,         /* pattern { body }   or  { body }        */
    TOP_FN,           /* type fn name(params) { body }          */
    TOP_STMT,         /* bare statement (inline / REPL)         */
} TopKind;


typedef struct {
    TopKind kind;
    Loc     loc;

    union {
        /* TOP_BEGIN / TOP_END */
        struct { Stmt *body; } begin_end;

        /* TOP_RULE */
        struct {
            Expr *pattern;    /* NULL = match every record      */
            Stmt *body;
        } rule;

        /* TOP_FN — mirrors STMT_FN_DECL at top level */
        struct {
            uint8_t  return_type;
            xf_Str  *name;
            Param   *params;
            size_t   param_count;
            Stmt    *body;
        } fn;

        /* TOP_STMT */
        struct { Stmt *stmt; } stmt;

    } as;

} TopLevel;


/* ============================================================
 * Program — root of the AST
 * ============================================================ */

typedef struct {
    TopLevel **items;
    size_t     count;
    size_t     capacity;
    const char *source;   /* filename or "<inline>" or "<repl>" */
} Program;


/* ============================================================
 * AST allocation / free
 * ============================================================ */

/* expression constructors */
Expr *ast_num(double val, Loc loc);
Expr *ast_str(xf_Str *val, Loc loc);
Expr *ast_regex(xf_Str *pattern, uint32_t flags, Loc loc);
Expr *ast_arr_lit(Expr **items, size_t count, Loc loc);
Expr *ast_map_lit(Expr **keys, Expr **vals, size_t count, Loc loc);
Expr *ast_set_lit(Expr **items, size_t count, Loc loc);
Expr *ast_ident(xf_Str *name, Loc loc);
Expr *ast_field(int index, Loc loc);
Expr *ast_ivar(TokenKind var, Loc loc);
Expr *ast_svar(TokenKind var, Loc loc);
Expr *ast_unary(UnOp op, Expr *operand, Loc loc);
Expr *ast_binary(BinOp op, Expr *left, Expr *right, Loc loc);
Expr *ast_ternary(Expr *cond, Expr *then, Expr *els, Loc loc);
Expr *ast_coalesce(Expr *left, Expr *right, Loc loc);
Expr *ast_assign(AssignOp op, Expr *target, Expr *value, Loc loc);
Expr *ast_walrus(xf_Str *name, uint8_t type, Expr *value, Loc loc);
Expr *ast_call(Expr *callee, Expr **args, size_t argc, Loc loc);
Expr *ast_subscript(Expr *obj, Expr *key, Loc loc);
/* output format modes for STMT_OUTFMT / RecordCtx.out_mode */
#define XF_OUTFMT_TEXT  0
#define XF_OUTFMT_CSV   1
#define XF_OUTFMT_TSV   2
#define XF_OUTFMT_JSON  3

Expr *ast_member(Expr *obj, xf_Str *field, Loc loc);
Expr *ast_state(Expr *operand, Loc loc);
Expr *ast_type(Expr *operand, Loc loc);
Expr *ast_len(Expr *operand, Loc loc);
Expr *ast_cast(uint8_t to_type, Expr *operand, Loc loc);
Expr *ast_pipe_fn(Expr *left, Expr *right, Loc loc);
Expr *ast_spread(Expr *operand, Loc loc);
Expr *ast_fn(uint8_t ret, Param *params, size_t pc, Stmt *body, Loc loc);

/* statement constructors */
Stmt *ast_block(Stmt **stmts, size_t count, Loc loc);
Stmt *ast_expr_stmt(Expr *expr, Loc loc);
Stmt *ast_var_decl(uint8_t type, xf_Str *name, Expr *init, Loc loc);
Stmt *ast_fn_decl(uint8_t ret, xf_Str *name,
                  Param *params, size_t pc, Stmt *body, Loc loc);
Stmt *ast_if(Branch *branches, size_t count, Stmt *els, Loc loc);
Stmt *ast_while(Expr *cond, Stmt *body, Loc loc);
Stmt *ast_for(xf_Str *iter, Expr *collection, Stmt *body, Loc loc);
Stmt *ast_while_short(Expr *cond, Stmt *body, Loc loc);
Stmt *ast_for_short(Expr *collection, xf_Str *iter, Stmt *body, Loc loc);
Stmt *ast_return(Expr *value, Loc loc);
Stmt *ast_next(Loc loc);
Stmt *ast_exit(Loc loc);
Stmt *ast_print(Expr **args, size_t count, Expr *redirect, Loc loc);
Stmt *ast_printf_stmt(Expr **args, size_t count, Expr *redirect, Loc loc);
Stmt *ast_outfmt(uint8_t mode, Loc loc);
Stmt *ast_import(xf_Str *path, Loc loc);
Stmt *ast_delete(Expr *target, Loc loc);
Stmt *ast_spawn(Expr *call, Loc loc);
Stmt *ast_join(Expr *handle, Loc loc);
Stmt *ast_subst(xf_Str *pat, xf_Str *rep, uint32_t flags, Expr *tgt, Loc loc);
Stmt *ast_trans(xf_Str *from, xf_Str *to, Expr *target, Loc loc);

/* top-level constructors */
TopLevel *ast_top_begin(Stmt *body, Loc loc);
TopLevel *ast_top_end(Stmt *body, Loc loc);
TopLevel *ast_top_rule(Expr *pattern, Stmt *body, Loc loc);
TopLevel *ast_top_fn(uint8_t ret, xf_Str *name,
                     Param *params, size_t pc, Stmt *body, Loc loc);
TopLevel *ast_top_stmt(Stmt *stmt, Loc loc);

/* program */
Program  *ast_program_new(const char *source);
void      ast_program_push(Program *p, TopLevel *item);
void      ast_program_free(Program *p);

/* recursive free */
void      ast_expr_free(Expr *e);
void      ast_stmt_free(Stmt *s);
void      ast_top_free(TopLevel *t);

/* debug print (indented) */
void      ast_expr_print(const Expr *e, int indent);
void      ast_stmt_print(const Stmt *s, int indent);
void      ast_program_print(const Program *p);

#endif /* XF_AST_H */
