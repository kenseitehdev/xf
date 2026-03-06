#ifndef XF_INTERP_H
#define XF_INTERP_H

#include "ast.h"
#include "symTable.h"
#include "vm.h"

/* ============================================================
 * xf interpreter
 *
 * Two modes:
 *
 *   Tree-walk (interp_eval_*):
 *     Directly evaluates AST nodes against a SymTable.
 *     Used by the REPL for fast turn-around — no compile step.
 *
 *   Compiler (interp_compile_*):
 *     Walks the AST and emits bytecode into a Chunk.
 *     Used for scripts and hot pattern-action rules.
 *     The VM then executes the Chunk.
 *
 * The REPL uses tree-walk for top-level expressions and
 * switches to compiled mode inside fn bodies that are called
 * more than once (future optimisation — stub for now).
 * ============================================================ */


/* ------------------------------------------------------------
 * Interpreter context
 * ------------------------------------------------------------ */

typedef struct {
    SymTable *syms;
    VM       *vm;
    bool      had_error;
    char      err_msg[512];

    /* current function return value (for tree-walk) */
    bool      returning;
    xf_Value  return_val;

    /* nexting / exiting */
    bool      nexting;
    bool      exiting;
} Interp;


/* ------------------------------------------------------------
 * Init / free
 * ------------------------------------------------------------ */

void interp_init(Interp *it, SymTable *syms, VM *vm);
void interp_free(Interp *it);


/* ------------------------------------------------------------
 * Tree-walk evaluation (REPL / one-shot)
 * ------------------------------------------------------------ */

/* evaluate a full program in tree-walk mode */
int  interp_run_program(Interp *it, Program *prog);
void interp_run_end(Interp *it, Program *prog);

/* feed one input record through all TOP_RULE items.
 * Call this once per input line, between run_program's BEGIN and END phases. */
void interp_feed_record(Interp *it, Program *prog,
                        const char *rec, size_t len);

/* evaluate one top-level item */
xf_Value interp_eval_top(Interp *it, TopLevel *top);

/* evaluate a statement; returns last value or NULL state */
xf_Value interp_eval_stmt(Interp *it, Stmt *s);

/* evaluate an expression */
xf_Value interp_eval_expr(Interp *it, Expr *e);


/* ------------------------------------------------------------
 * Compiler (AST → Chunk)
 * ------------------------------------------------------------ */

/* compile a full program into vm->rules / begin_chunk / end_chunk */
bool interp_compile_program(Interp *it, Program *prog);

/* compile a single expression into a fresh Chunk */
Chunk *interp_compile_expr(Interp *it, Expr *e, const char *name);

/* compile a statement block into a Chunk */
Chunk *interp_compile_stmt(Interp *it, Stmt *s, const char *name);


/* ------------------------------------------------------------
 * Built-in function dispatch
 * ------------------------------------------------------------ */

xf_Value interp_call_builtin(Interp *it, const char *name,
                              xf_Value *args, size_t argc);


/* ------------------------------------------------------------
 * Error helpers
 * ------------------------------------------------------------ */

void interp_error(Interp *it, Loc loc, const char *fmt, ...);
void interp_type_err(Interp *it, Loc loc,
                     const char *op, uint8_t got, uint8_t expected);

#endif /* XF_INTERP_H */

