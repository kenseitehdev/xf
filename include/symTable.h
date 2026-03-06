#ifndef XF_SYMTABLE_H
#define XF_SYMTABLE_H

#include "value.h"
#include "lexer.h"
#include <stddef.h>
#include <stdbool.h>

/* ============================================================
 * xf symbol table
 *
 * Scoped chain of hash tables.
 * Each scope is a flat array of Symbol entries (open addressing).
 * Scopes are linked: child → parent → ... → global.
 *
 * Symbols carry both static type info (for the parser/checker)
 * and runtime value (for the interpreter).
 *
 * Functions are symbols too — type XF_TYPE_FN, value = fn ptr.
 * ============================================================ */


/* ------------------------------------------------------------
 * Symbol kinds
 * ------------------------------------------------------------ */

typedef enum {
    SYM_VAR,       /* regular variable                          */
    SYM_FN,        /* function declaration                      */
    SYM_PARAM,     /* function parameter                        */
    SYM_BUILTIN,   /* built-in function or value                */
} SymKind;


/* ------------------------------------------------------------
 * Symbol
 * ------------------------------------------------------------ */

typedef struct Symbol {
    xf_Str  *name;
    SymKind  kind;
    uint8_t  type;        /* declared XF_TYPE_*                 */
    uint8_t  state;       /* current XF_STATE_*                 */
    bool     is_const;    /* immutable after first assignment    */
    bool     is_defined;  /* false = declared but not assigned   */
    xf_Value value;       /* runtime value (interpreter use)    */
    Loc      decl_loc;    /* where it was declared               */
} Symbol;


/* ------------------------------------------------------------
 * Scope — one level of the symbol table
 * ------------------------------------------------------------ */

#define SCOPE_INIT_CAP 16

typedef enum {
    SCOPE_GLOBAL,     /* top-level                              */
    SCOPE_FN,         /* function body                          */
    SCOPE_BLOCK,      /* bare { } block                         */
    SCOPE_LOOP,       /* for / while body (enables next/break)  */
    SCOPE_PATTERN,    /* pattern-action rule body               */
} ScopeKind;

typedef struct Scope {
    ScopeKind    kind;
    Symbol      *entries;    /* flat array, open addressing     */
    size_t       count;
    size_t       capacity;
    struct Scope *parent;    /* enclosing scope, NULL = global  */
    uint8_t      fn_ret_type; /* expected return type if SCOPE_FN */
} Scope;


/* ------------------------------------------------------------
 * SymTable — the full table (root scope + scope stack)
 * ------------------------------------------------------------ */

typedef struct {
    Scope  *current;    /* innermost active scope              */
    Scope  *global;     /* always the outermost scope          */
    size_t  depth;      /* nesting depth (0 = global)          */

    /* error reporting */
    bool    had_error;
    char    err_msg[256];
    Loc     err_loc;
} SymTable;


/* ------------------------------------------------------------
 * SymTable API
 * ------------------------------------------------------------ */

/* init global scope */
void     sym_init(SymTable *st);

/* free all scopes and their entries */
void     sym_free(SymTable *st);

/* push a new child scope */
Scope   *sym_push(SymTable *st, ScopeKind kind);

/* pop innermost scope (does not free — caller may inspect) */
Scope   *sym_pop(SymTable *st);

/* free a scope and its entries */
void     scope_free(Scope *sc);


/* ------------------------------------------------------------
 * Symbol operations
 * ------------------------------------------------------------ */

/*
 * sym_declare — add a new symbol to the current scope.
 * Returns NULL and sets had_error if name already declared
 * in the current scope (shadowing outer scopes is allowed).
 */
Symbol  *sym_declare(SymTable *st, xf_Str *name, SymKind kind,
                     uint8_t type, Loc loc);

/*
 * sym_lookup — search current scope outward to global.
 * Returns NULL if not found.
 */
Symbol  *sym_lookup(SymTable *st, const char *name, size_t len);
Symbol  *sym_lookup_str(SymTable *st, xf_Str *name);

/*
 * sym_lookup_local — search only the current scope.
 */
Symbol  *sym_lookup_local(SymTable *st, const char *name, size_t len);

/*
 * sym_assign — update value and state of an existing symbol.
 * Returns false if symbol not found or is_const.
 */
bool     sym_assign(SymTable *st, xf_Str *name, xf_Value val);

/*
 * sym_define_builtin — add a built-in at global scope.
 */
Symbol  *sym_define_builtin(SymTable *st, const char *name,
                             uint8_t type, xf_Value val);


/* ------------------------------------------------------------
 * Scope query helpers
 * ------------------------------------------------------------ */

/* true if currently inside a function scope */
bool     sym_in_fn(const SymTable *st);

/* return the nearest enclosing fn scope, or NULL */
Scope   *sym_fn_scope(SymTable *st);

/* true if currently inside a loop scope */
bool     sym_in_loop(const SymTable *st);

/* expected return type of enclosing function */
uint8_t  sym_fn_return_type(const SymTable *st);


/* ------------------------------------------------------------
 * Built-in registration — called once at startup
 * ------------------------------------------------------------ */

void     sym_register_builtins(SymTable *st);


/* ------------------------------------------------------------
 * Debug
 * ------------------------------------------------------------ */

void     sym_print_scope(const Scope *sc);
void     sym_print_all(const SymTable *st);

#endif /* XF_SYMTABLE_H */
