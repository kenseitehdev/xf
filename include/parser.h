#ifndef XF_PARSER_H
#define XF_PARSER_H

#include "lexer.h"
#include "ast.h"
#include "symTable.h"

/* ============================================================
 * xf parser — recursive descent
 *
 * Walks lex->tokens[] by index. No allocation in the lexer
 * during parsing — all nodes are allocated by the ast_* helpers.
 *
 * Operator precedence (low → high):
 *   1.  assignment        = += -= *= /= %= ..=  :=
 *   2.  ternary           ? :
 *   3.  coalescing        ??
 *   4.  logical or        ||
 *   5.  logical and       &&
 *   6.  regex match       ~  !~
 *   7.  equality          ==  !=
 *   8.  comparison        <  >  <=  >=  <=>
 *   9.  concat            ..
 *  10.  additive          +  -
 *  11.  multiplicative    *  /  %
 *  12.  exponent          ^  (right-associative)
 *  13.  unary             -  !  ++  --
 *  14.  postfix           x++  x--  f()  a[]  x.f  x.state  x.type
 *  15.  primary           literal  ident  ( expr )  $n
 * ============================================================ */


/* ------------------------------------------------------------
 * Parser state
 * ------------------------------------------------------------ */

typedef struct {
    Lexer    *lex;          /* token source (already tokenized) */
    size_t    pos;          /* current index into lex->tokens   */
    SymTable *syms;         /* symbol table (shared with caller) */

    /* error state */
    bool      had_error;
    bool      panic_mode;   /* true = skip tokens until sync point */
    char      err_msg[512];
    Loc       err_loc;

} Parser;


/* ------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------ */

/*
 * parse — full parse of a tokenized Lexer.
 * Populates syms with declarations found.
 * Returns a heap-allocated Program (caller owns, use ast_program_free).
 * Returns NULL on unrecoverable error.
 */
Program *parse(Lexer *lex, SymTable *syms);

/*
 * parse_repl_line — parse one REPL input line or block.
 * Returns a single TopLevel item or NULL on error.
 * Used by the REPL to process input incrementally.
 */
TopLevel *parse_repl_line(Lexer *lex, SymTable *syms);

/*
 * parse_expr_str — parse a single expression from a C string.
 * Useful for testing and -e single-expression mode.
 */
Expr *parse_expr_str(const char *src, SymTable *syms);


/* ------------------------------------------------------------
 * Internal parse functions (exposed for testing)
 * ------------------------------------------------------------ */

/* top-level */
TopLevel *parse_top(Parser *p);
TopLevel *parse_fn_decl_top(Parser *p);
TopLevel *parse_rule(Parser *p);

/* statements */
Stmt *parse_stmt(Parser *p);
Stmt *parse_block(Parser *p);
Stmt *parse_fn_decl(Parser *p);
Stmt *parse_var_decl(Parser *p, uint8_t type);
Stmt *parse_if(Parser *p);
Stmt *parse_while(Parser *p);
Stmt *parse_for(Parser *p);
Stmt *parse_return(Parser *p);
Stmt *parse_print(Parser *p);
Stmt *parse_spawn(Parser *p);
Stmt *parse_join(Parser *p);
Stmt *parse_import(Parser *p);

/* expressions — precedence levels */
Expr *parse_expr(Parser *p);
Expr *parse_assign(Parser *p);
Expr *parse_ternary(Parser *p);
Expr *parse_coalesce(Parser *p);
Expr *parse_or(Parser *p);
Expr *parse_and(Parser *p);
Expr *parse_match(Parser *p);
Expr *parse_equality(Parser *p);
Expr *parse_compare(Parser *p);
Expr *parse_concat(Parser *p);
Expr *parse_add(Parser *p);
Expr *parse_mul(Parser *p);
Expr *parse_exp(Parser *p);
Expr *parse_unary(Parser *p);
Expr *parse_postfix(Parser *p);
Expr *parse_primary(Parser *p);

/* call argument list */
Expr **parse_arglist(Parser *p, size_t *out_count);

/* parameter list (for fn declarations) */
Param *parse_paramlist(Parser *p, size_t *out_count);

/* type keyword → XF_TYPE_* */
uint8_t parse_type(Parser *p);

#endif /* XF_PARSER_H */
