#include "../include/symTable.h"
#include "../include/value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Scope alloc / free
 * ============================================================ */

static Scope *scope_new(ScopeKind kind, Scope *parent, uint8_t fn_ret) {
    Scope *sc       = calloc(1, sizeof(Scope));
    sc->kind        = kind;
    sc->capacity    = SCOPE_INIT_CAP;
    sc->entries     = calloc(sc->capacity, sizeof(Symbol));
    sc->parent      = parent;
    sc->fn_ret_type = fn_ret;
    return sc;
}

void scope_free(Scope *sc) {
    if (!sc) return;
    for (size_t i = 0; i < sc->capacity; i++) {
        if (sc->entries[i].name) {
            xf_str_release(sc->entries[i].name);
            xf_value_release(sc->entries[i].value);  /* drop owned ref */
        }
    }
    free(sc->entries);
    free(sc);
}


/* ============================================================
 * SymTable init / free
 * ============================================================ */

void sym_init(SymTable *st) {
    memset(st, 0, sizeof(*st));
    st->global  = scope_new(SCOPE_GLOBAL, NULL, XF_TYPE_VOID);
    st->current = st->global;
    st->depth   = 0;
}

void sym_free(SymTable *st) {
    /* walk back from current to global, freeing each scope */
    Scope *sc = st->current;
    while (sc) {
        Scope *parent = sc->parent;
        scope_free(sc);
        sc = parent;
    }
    st->current = NULL;
    st->global  = NULL;
}


/* ============================================================
 * Scope push / pop
 * ============================================================ */

Scope *sym_push(SymTable *st, ScopeKind kind) {
    uint8_t fn_ret = kind == SCOPE_FN
                         ? XF_TYPE_VOID        /* caller sets fn_ret_type */
                         : st->current->fn_ret_type;
    Scope *sc   = scope_new(kind, st->current, fn_ret);
    st->current = sc;
    st->depth++;
    return sc;
}

Scope *sym_pop(SymTable *st) {
    if (st->current == st->global) return st->global;
    Scope *popped = st->current;
    st->current   = popped->parent;
    st->depth--;
    return popped;
}


/* ============================================================
 * Internal hash lookup within a single scope
 * ============================================================ */

static uint32_t hash_str(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static Symbol *scope_find(Scope *sc, const char *name, size_t len) {
    if (sc->count == 0) return NULL;
    uint32_t h   = hash_str(name, len);
    size_t   idx = h & (sc->capacity - 1);
    for (size_t probe = 0; probe < sc->capacity; probe++) {
        Symbol *s = &sc->entries[idx];
        if (!s->name) return NULL;   /* empty slot = not found */
        if (s->name->len == len &&
            memcmp(s->name->data, name, len) == 0)
            return s;
        idx = (idx + 1) & (sc->capacity - 1);
    }
    return NULL;
}

static Symbol *scope_insert(Scope *sc, xf_Str *name) {
    /* grow if load factor > 0.75 */
    if (sc->count * 4 >= sc->capacity * 3) {
        size_t   new_cap  = sc->capacity * 2;
        Symbol  *new_ents = calloc(new_cap, sizeof(Symbol));
        for (size_t i = 0; i < sc->capacity; i++) {
            if (!sc->entries[i].name) continue;
            uint32_t h   = xf_str_hash(sc->entries[i].name);
            size_t   idx = h & (new_cap - 1);
            while (new_ents[idx].name)
                idx = (idx + 1) & (new_cap - 1);
            new_ents[idx] = sc->entries[i];
        }
        free(sc->entries);
        sc->entries  = new_ents;
        sc->capacity = new_cap;
    }

    uint32_t h   = xf_str_hash(name);
    size_t   idx = h & (sc->capacity - 1);
    while (sc->entries[idx].name)
        idx = (idx + 1) & (sc->capacity - 1);

    sc->entries[idx].name = xf_str_retain(name);
    sc->count++;
    return &sc->entries[idx];
}


/* ============================================================
 * Symbol operations
 * ============================================================ */

Symbol *sym_declare(SymTable *st, xf_Str *name, SymKind kind,
                    uint8_t type, Loc loc) {
    /* check for duplicate in current scope only */
    Symbol *existing = scope_find(st->current, name->data, name->len);
    if (existing) {
        st->had_error = true;
        st->err_loc   = loc;
        snprintf(st->err_msg, sizeof(st->err_msg),
                 "'%s' already declared in this scope (previous at %s:%u:%u)",
                 name->data,
                 existing->decl_loc.source,
                 existing->decl_loc.line,
                 existing->decl_loc.col);
        return NULL;
    }

    Symbol *sym  = scope_insert(st->current, name);
    sym->kind     = kind;
    sym->type     = type;
    sym->state    = XF_STATE_UNDEF;
    sym->decl_loc = loc;
    sym->value    = xf_val_undef(type);
    return sym;
}

Symbol *sym_lookup(SymTable *st, const char *name, size_t len) {
    Scope *sc = st->current;
    while (sc) {
        Symbol *s = scope_find(sc, name, len);
        if (s) return s;
        sc = sc->parent;
    }
    return NULL;
}

Symbol *sym_lookup_str(SymTable *st, xf_Str *name) {
    return sym_lookup(st, name->data, name->len);
}

Symbol *sym_lookup_local(SymTable *st, const char *name, size_t len) {
    return scope_find(st->current, name, len);
}

bool sym_assign(SymTable *st, xf_Str *name, xf_Value val) {
    Symbol *sym = sym_lookup_str(st, name);
    if (!sym) {
        st->had_error = true;
        snprintf(st->err_msg, sizeof(st->err_msg),
                 "assignment to undeclared variable '%s'", name->data);
        return false;
    }
    if (sym->is_const && sym->is_defined) {
        st->had_error = true;
        snprintf(st->err_msg, sizeof(st->err_msg),
                 "'%s' is immutable", name->data);
        return false;
    }
    xf_value_release(sym->value);  /* drop old owned ref */
    sym->value      = val;          /* steal new owned ref */
    sym->state      = val.state;
    sym->is_defined = true;
    return true;
}

Symbol *sym_define_builtin(SymTable *st, const char *name,
                            uint8_t type, xf_Value val) {
    xf_Str *s   = xf_str_from_cstr(name);
    Symbol *sym = scope_insert(st->global, s);
    xf_str_release(s);
    sym->kind       = SYM_BUILTIN;
    sym->type       = type;
    sym->state      = XF_STATE_OK;
    sym->is_const   = true;
    sym->is_defined = true;
    sym->value      = val;
    return sym;
}


/* ============================================================
 * Scope query helpers
 * ============================================================ */

bool sym_in_fn(const SymTable *st) {
    Scope *sc = st->current;
    while (sc) {
        if (sc->kind == SCOPE_FN) return true;
        sc = sc->parent;
    }
    return false;
}

Scope *sym_fn_scope(SymTable *st) {
    Scope *sc = st->current;
    while (sc) {
        if (sc->kind == SCOPE_FN) return sc;
        sc = sc->parent;
    }
    return NULL;
}

bool sym_in_loop(const SymTable *st) {
    Scope *sc = st->current;
    while (sc && sc->kind != SCOPE_FN) {
        if (sc->kind == SCOPE_LOOP) return true;
        sc = sc->parent;
    }
    return false;
}

uint8_t sym_fn_return_type(const SymTable *st) {
    Scope *sc = sym_fn_scope((SymTable *)st);
    return sc ? sc->fn_ret_type : XF_TYPE_VOID;
}


/* ============================================================
 * Built-in registration
 * ============================================================ */

/* Table of built-in names.  Adding a new built-in only requires one
 * entry here — no other changes needed.                             */
static const char *const k_builtins[] = {
    /* math */
    "sin", "cos", "sqrt", "abs", "int",
    /* string */
    "len", "split", "sub", "gsub", "match", "substr", "index",
    "toupper", "tolower", "trim", "sprintf", "column",
    /* I/O */
    "getline", "close", "flush",
    /* collections */
    "push", "pop", "shift", "unshift", "remove", "has", "keys", "values",
    "read", "lines", "write", "append",
    /* system */
    "system", "time", "rand", "srand", "exit",
};
#define BUILTIN_COUNT (sizeof(k_builtins) / sizeof(k_builtins[0]))

void sym_register_builtins(SymTable *st) {
    xf_Value fn_undef = xf_val_undef(XF_TYPE_FN);
    for (size_t i = 0; i < BUILTIN_COUNT; i++)
        sym_define_builtin(st, k_builtins[i], XF_TYPE_FN, fn_undef);
}


/* ============================================================
 * Debug
 * ============================================================ */

static const char *scope_kind_name(ScopeKind k) {
    switch (k) {
        case SCOPE_GLOBAL:  return "global";
        case SCOPE_FN:      return "fn";
        case SCOPE_BLOCK:   return "block";
        case SCOPE_LOOP:    return "loop";
        case SCOPE_PATTERN: return "pattern";
        default:            return "?";
    }
}

void sym_print_scope(const Scope *sc) {
    printf("scope[%s] (%zu entries)\n", scope_kind_name(sc->kind), sc->count);
    for (size_t i = 0; i < sc->capacity; i++) {
        const Symbol *s = &sc->entries[i];
        if (!s->name) continue;
        printf("  %-20s  type=%-6s  state=%-13s  %s\n",
               s->name->data,
               XF_TYPE_NAMES[s->type < XF_TYPE_COUNT ? s->type : 0],
               XF_STATE_NAMES[s->state < XF_STATE_COUNT ? s->state : 0],
               s->is_const ? "const" : "");
    }
}

void sym_print_all(const SymTable *st) {
    Scope *sc = st->current;
    int depth = (int)st->depth;
    while (sc) {
        printf("[depth %d] ", depth--);
        sym_print_scope(sc);
        sc = sc->parent;
    }
}