#if defined(__linux__) || defined(__CYGWIN__)
#define _GNU_SOURCE
#endif
#include "../include/interp.h"
#include "../include/parser.h"
#include "../include/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <regex.h>
#include <strings.h>
#include <pthread.h>
/* ============================================================
 * Spawn thread infrastructure
 *
 * Each `spawn fn(args)` statement launches a pthread.
 * The pthread receives a SpawnCtx containing:
 *   - a pre-evaluated xf_Value fn value (owned ref)
 *   - pre-evaluated arg values (owned refs)
 *   - a fresh SymTable/Interp that shares the parent VM
 * A global mutex protects VM state mutations (record context,
 * globals, redir cache) during concurrent execution.
 * The join handle is a uint32 thread id mapping into g_spawn[].
 * ============================================================ */

#define XF_SPAWN_MAX 256

typedef struct {
    pthread_t   tid;
    uint32_t    id;          /* handle value returned to XF code */
    bool        done;
    xf_Value    result;
    xf_Value    fn_val;      /* owned ref to the fn being called */
    xf_Value    args[64];
    size_t      argc;
    VM         *vm;          /* shared VM — access guarded by g_spawn_mu */
    SymTable   *parent_syms; /* borrowed — for copying globals into thread */
} SpawnCtx;

static pthread_mutex_t g_spawn_mu    = PTHREAD_MUTEX_INITIALIZER;
static SpawnCtx        g_spawn[XF_SPAWN_MAX];
static size_t          g_spawn_count = 0;
static uint32_t        g_spawn_next  = 1;  /* 0 reserved for "no handle" */
static void copy_globals_from(SymTable *dst, SymTable *src);
static void *spawn_thread_fn(void *arg) {
    SpawnCtx *ctx = (SpawnCtx *)arg;

    /* Private SymTable+Interp sharing only the VM.
     * Parent globals copied in so core.*, user globals visible. */
    SymTable  syms;
    sym_init(&syms);
    if (ctx->parent_syms) copy_globals_from(&syms, ctx->parent_syms);

    /* Initialise a private Interp WITHOUT calling core_set_fn_caller.
     * Worker threads must not overwrite the global g_fn_caller_syms with
     * their short-lived private SymTable pointer; doing so would cause
     * any concurrent or later core.process / core.ds call to use a
     * dangling pointer once this thread's SymTable is freed. */
    Interp it;
    memset(&it, 0, sizeof(it));
    it.syms     = &syms;
    it.vm       = ctx->vm;
    it.last_err = xf_val_null();

    /* Snapshot the record context so this thread sees the record
     * that was live at spawn time, independent of main thread updates */
    vm_rec_snapshot(ctx->vm, &it.rec_snap);
    it.use_rec_snap = true;

    if (ctx->fn_val.state == XF_STATE_OK &&
        ctx->fn_val.type  == XF_TYPE_FN  &&
        ctx->fn_val.data.fn) {
        xf_Fn *fn = ctx->fn_val.data.fn;
        if (!fn->is_native) {
            Scope *fn_sc = sym_push(&syms, SCOPE_FN);
            fn_sc->fn_ret_type = fn->return_type;
            for (size_t i = 0; i < fn->param_count; i++) {
                xf_Value av = i < ctx->argc ? ctx->args[i]
                                            : xf_val_undef(fn->params[i].type);
                Symbol *ps = sym_declare(&syms, fn->params[i].name,
                                         SYM_PARAM, fn->params[i].type,
                                         (Loc){.source="<spawn>",.line=0,.col=0});
                if (ps) { ps->value = xf_value_retain(av); ps->state = av.state; ps->is_defined = true; }
            }

            it.returning = false;
            interp_eval_stmt(&it, (Stmt *)fn->body);
            ctx->result = it.returning ? it.return_val : xf_val_null();
            it.returning = false;

            scope_free(sym_pop(&syms));
        } else if (fn->native_v) {
            ctx->result = fn->native_v(ctx->args, ctx->argc);
        }
    }

    /* release args */
    for (size_t i = 0; i < ctx->argc; i++) xf_value_release(ctx->args[i]);
    xf_value_release(ctx->fn_val);

    interp_free(&it);
    vm_rec_snapshot_free(&it.rec_snap);
    sym_free(&syms);

    pthread_mutex_lock(&g_spawn_mu);
    ctx->done = true;
    pthread_mutex_unlock(&g_spawn_mu);
    return NULL;
}
static inline xf_Value propagate_err(xf_Value v) { return v; }

/* ── copy_globals_from ───────────────────────────────────────────
 * Copy all symbols from src's global scope into dst's global scope,
 * retaining each value.  Called before executing an XF fn in a
 * worker thread so that core.*, user-defined globals, etc. are
 * visible without sharing the parent SymTable struct.
 * dst must have been freshly sym_init'd (empty global scope).
 * This is a shallow copy: values are ref-counted, no double-free risk.
 * Caller must hold no lock — this reads parent globals which are
 * stable (not mutated) during parallel worker execution.
 * ---------------------------------------------------------------- */
static void copy_globals_from(SymTable *dst, SymTable *src) {
    if (!src || !src->global) return;
    Scope *sg = src->global;
    for (size_t i = 0; i < sg->capacity; i++) {
        Symbol *s = &sg->entries[i];
        if (!s->name) continue;   /* empty slot */
        Symbol *ds = sym_declare(dst, s->name, s->kind, s->type, s->decl_loc);
        if (ds) {
            ds->value      = xf_value_retain(s->value);
            ds->state      = s->state;
            ds->is_const   = s->is_const;
            ds->is_defined = s->is_defined;
        }
    }
}

/* ── interp_exec_xf_fn ───────────────────────────────────────────
 * Executes an XF-language (or native) fn in the calling thread.
 * Creates a private SymTable+Interp that shares only the VM pointer.
 * Parent globals are COPIED into the private table so that core.*,
 * user-defined functions and variables are visible to the worker.
 * All mutations (local vars, fn scope) stay private — no races.
 * ---------------------------------------------------------------- */
static xf_Value interp_exec_xf_fn(void *vm_ptr, void *syms_ptr,
                                   xf_fn_t *fn, xf_Value *args, size_t argc) {
    if (!fn) return xf_val_null();
    if (fn->is_native && fn->native_v) return fn->native_v(args, argc);
    if (!fn->body) return xf_val_null();

    VM       *vm         = (VM *)vm_ptr;
    SymTable *parent_st  = (SymTable *)syms_ptr;

    /* private symbol table — copy parent globals in, no shared state */
    SymTable syms;
    sym_init(&syms);
    if (parent_st) copy_globals_from(&syms, parent_st);

    /* Initialise the private Interp WITHOUT calling core_set_fn_caller.
     * Re-registering here would clobber g_fn_caller_syms with this
     * thread's private (and short-lived) SymTable pointer, causing any
     * concurrent or subsequent thread calling g_fn_caller to dereference
     * a dangling pointer — the exact source of the NAV-return bug. */
    Interp it;
    memset(&it, 0, sizeof(it));
    it.syms     = &syms;
    it.vm       = vm;
    it.last_err = xf_val_null();

    /* Snapshot record context — worker sees the record at call time */
    vm_rec_snapshot(vm, &it.rec_snap);
    it.use_rec_snap = true;

    /* push fn scope and bind parameters — all private */
    Scope *fn_sc = sym_push(&syms, SCOPE_FN);
    fn_sc->fn_ret_type = fn->return_type;
    for (size_t i = 0; i < fn->param_count; i++) {
        xf_Value av = (i < argc) ? args[i]
                                 : xf_val_undef(fn->params[i].type);
        Symbol *ps = sym_declare(&syms, fn->params[i].name,
                                  SYM_PARAM, fn->params[i].type,
                                  (Loc){.source="<worker>",.line=0,.col=0});
        if (ps) { ps->value = xf_value_retain(av); ps->state = av.state; ps->is_defined = true; }
    }

    it.returning = false;
    interp_eval_stmt(&it, (Stmt *)fn->body);
    xf_Value result = it.returning ? it.return_val : xf_val_null();
    it.returning = false;

    scope_free(sym_pop(&syms));
    interp_free(&it);
    vm_rec_snapshot_free(&it.rec_snap);
    sym_free(&syms);
    return result;
}
static bool bind_loop_value(Interp *it, LoopBind *bind, xf_Value v, Loc loc);

static bool bind_loop_tuple(Interp *it, LoopBind *bind, xf_Value v, Loc loc) {
    if (!bind) return true;
    if (bind->kind != LOOP_BIND_TUPLE) {
        interp_error(it, loc, "internal error: expected tuple loop binding");
        return false;
    }

    if (v.state != XF_STATE_OK || v.type != XF_TYPE_TUPLE || !v.data.tuple) {
        interp_error(it, loc, "cannot destructure non-tuple value");
        return false;
    }

    size_t need = bind->as.tuple.count;
    size_t have = xf_tuple_len(v.data.tuple);

    if (have != need) {
        interp_error(it, loc, "tuple destructuring arity mismatch: expected %zu values, got %zu",
                     need, have);
        return false;
    }

    for (size_t i = 0; i < need; i++) {
        xf_Value elem = xf_tuple_get(v.data.tuple, i);
        if (!bind_loop_value(it, bind->as.tuple.items[i], elem, loc))
            return false;
    }

    return true;
}

static bool bind_loop_value(Interp *it, LoopBind *bind, xf_Value v, Loc loc) {
    if (!bind) return true;

    switch (bind->kind) {
        case LOOP_BIND_NAME: {
            Symbol *sym = sym_declare(it->syms, bind->as.name,
                                      SYM_VAR, XF_TYPE_VOID, loc);
            if (!sym) {
                interp_error(it, loc, "failed to bind loop variable '%s'",
                             bind->as.name ? bind->as.name->data : "<null>");
                return false;
            }

            xf_value_release(sym->value);
            sym->value = xf_value_retain(v);
            sym->state = v.state;
            sym->is_defined = true;
            return true;
        }

        case LOOP_BIND_TUPLE:
            return bind_loop_tuple(it, bind, v, loc);

        default:
            interp_error(it, loc, "invalid loop binding");
            return false;
    }
}

static bool bind_loop_index_value(Interp *it,
                                  LoopBind *key_bind,
                                  LoopBind *val_bind,
                                  xf_Value keyv,
                                  xf_Value valv,
                                  Loc loc) {
    if (key_bind) {
        if (!bind_loop_value(it, key_bind, keyv, loc)) return false;
    }
    if (val_bind) {
        if (!bind_loop_value(it, val_bind, valv, loc)) return false;
    }
    return true;
}
void interp_init(Interp *it, SymTable *syms, VM *vm) {
    memset(it, 0, sizeof(*it));
    it->syms     = syms;
    it->vm       = vm;
    it->last_err = xf_val_null();
    /* register XF-fn execution callback so core.process / core.ds.stream
     * can call XF-language functions from pthreads with full global visibility */
    if (vm) core_set_fn_caller(vm, syms, interp_exec_xf_fn);
}
void interp_free(Interp *it) { (void)it; }

/* ──────────────────────────────────────────────────────────────────────
 * interp_call_core — delegate to a core.MODULE.FN native function.
 *
 * Looks up the `core` module in the symbol table, traverses to the
 * named sub-module and function, then calls fn->native_v directly.
 * Returns NAV(VOID) if the path cannot be resolved (module not loaded,
 * function not found, or not a native callable).
 * ────────────────────────────────────────────────────────────────────── */
static xf_Value interp_call_core(Interp *it,
                                   const char *module, const char *fn_name,
                                   xf_Value *args, size_t argc) {
    /* look up `core` in the symbol table */
    Symbol *core_sym = sym_lookup(it->syms, "core", 4);
    if (!core_sym || core_sym->value.type != XF_TYPE_MODULE ||
        !core_sym->value.data.mod) return xf_val_nav(XF_TYPE_VOID);

    /* traverse to sub-module */
    xf_Value sub = xf_module_get(core_sym->value.data.mod, module);
    if (sub.state != XF_STATE_OK || sub.type != XF_TYPE_MODULE || !sub.data.mod)
        return xf_val_nav(XF_TYPE_VOID);

    /* look up function */
    xf_Value fv = xf_module_get(sub.data.mod, fn_name);
    if (fv.state != XF_STATE_OK || fv.type != XF_TYPE_FN || !fv.data.fn)
        return xf_val_nav(XF_TYPE_VOID);

    xf_Fn *fn = fv.data.fn;
    if (!fn->is_native || !fn->native_v) return xf_val_nav(XF_TYPE_VOID);

    return fn->native_v(args, argc);
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

static char *xf_read_line_from_file(const char *path, int target_line) {
    if (!path || target_line <= 0) return NULL;
    if (strcmp(path, "<repl>") == 0) return NULL;

    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    char buf[4096];
    int line_no = 0;
    char *out = NULL;

    while (fgets(buf, sizeof(buf), fp)) {
        line_no++;
        if (line_no == target_line) {
            size_t len = strlen(buf);

            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
                buf[--len] = '\0';
            }

            out = malloc(len + 1);
            if (out) memcpy(out, buf, len + 1);
            break;
        }
    }

    fclose(fp);
    return out;
}

static void xf_print_caret_line(const char *src_line, int col) {
    fprintf(stdout, "   | ");

    if (!src_line || col <= 1) {
        fprintf(stdout, "^\n");
        return;
    }

    int visual = 0;
    for (int i = 0; src_line[i] && i < col - 1; i++) {
        if (src_line[i] == '\t') {
            fputc('\t', stdout);
        } else {
            fputc(' ', stdout);
        }
        visual++;
    }

    (void)visual;
    fprintf(stdout, "^\n");
}

void interp_error(Interp *it, Loc loc, const char *fmt, ...) {
    it->had_error = true;

    va_list ap;
    va_start(ap, fmt);

    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    va_end(ap);

    snprintf(it->err_msg, sizeof(it->err_msg), "%s", msg);

    fprintf(stdout, "ERR ──────────────────────────────────────\n");
    fprintf(stdout, "  %s\n", msg);

    if (loc.source) {
        fprintf(stdout, "  --> %s:%d:%d\n", loc.source, loc.line, loc.col);

        char *src_line = xf_read_line_from_file(loc.source, loc.line);
        if (src_line) {
            fprintf(stdout, "   |\n");
            fprintf(stdout, "%2d | %s\n", loc.line, src_line);
            xf_print_caret_line(src_line, loc.col);
            free(src_line);
        }
    }

    fprintf(stdout, "──────────────────────────────────────────\n");
}
static void xf_print_value(FILE *out, xf_Value v) {
    switch (v.state) {
        case XF_STATE_OK:
            break;
        default:
            fputs(XF_STATE_NAMES[v.state], out);
            return;
    }

    switch (v.type) {
        case XF_TYPE_NUM:
            fprintf(out, "%.15g", v.data.num);
            return;

        case XF_TYPE_STR:
            fputs(v.data.str ? v.data.str->data : "", out);
            return;

        case XF_TYPE_ARR: {
            fputc('[', out);
            if (v.data.arr) {
                for (size_t i = 0; i < v.data.arr->len; i++) {
                    if (i > 0) fputs(", ", out);
                    xf_print_value(out, v.data.arr->items[i]);
                }
            }
            fputc(']', out);
            return;
        }
case XF_TYPE_TUPLE: {
    fputc('(', out);
    if (v.data.tuple) {
        size_t n = xf_tuple_len(v.data.tuple);
        for (size_t i = 0; i < n; i++) {
            if (i > 0) fputs(", ", out);
            xf_print_value(out, xf_tuple_get(v.data.tuple, i));
        }
        if (n == 1) fputc(',', out);
    }
    fputc(')', out);
    return;
}
                case XF_TYPE_MAP: {
            fputc('{', out);
            if (v.data.map) {
                for (size_t i = 0; i < v.data.map->order_len; i++) {
                    if (i > 0) fputs(", ", out);
                    xf_Str *k = v.data.map->order[i];
                    fputs(k ? k->data : "", out);
                    fputc(':', out);
                    xf_Value mv = xf_map_get(v.data.map, k);
                    xf_print_value(out, mv);
                }
            }
            fputc('}', out);
            return;
        }

        case XF_TYPE_SET: {
            fputc('{', out);
            if (v.data.map) {
                for (size_t i = 0; i < v.data.map->order_len; i++) {
                    if (i > 0) fputs(", ", out);
                    xf_Str *k = v.data.map->order[i];
                    fputs(k ? k->data : "", out);
                }
            }
            fputc('}', out);
            return;
        }

        case XF_TYPE_FN:
            fputs("<fn>", out);
            return;

        case XF_TYPE_VOID:
            fputs("void", out);
            return;

        default: {
            xf_Value sv = xf_coerce_str(v);
            if (sv.state == XF_STATE_OK && sv.data.str)
                fputs(sv.data.str->data, out);
            else
                fputs(XF_TYPE_NAMES[v.type], out);
            xf_value_release(sv);
            return;
        }
        printf("\n\n\n");
    }
}
void interp_type_err(Interp *it, Loc loc,
                     const char *op, uint8_t got, uint8_t expected) {
    interp_error(it, loc, "type error in '%s': got %s, expected %s",
                 op, XF_TYPE_NAMES[got], XF_TYPE_NAMES[expected]);
}
static xf_Fn *build_fn(xf_Str *name, uint8_t ret,
                        Param *params, size_t pc, Stmt *body) {
    xf_Fn *fn = calloc(1, sizeof(xf_Fn));
    atomic_store(&fn->refcount, 1);   /* must be 1: xf_fn_release checks fetch_sub==1 */
    fn->name        = xf_str_retain(name);
    fn->return_type = ret;
    fn->param_count = pc;
    fn->body        = body;
    fn->is_native   = false;
    if (pc > 0) {
        fn->params = calloc(pc, sizeof(xf_param_t));
        for (size_t i = 0; i < pc; i++) {
            fn->params[i].name = xf_str_retain(params[i].name);
            fn->params[i].type = params[i].type;
        }
    }
    return fn;
}
static bool is_truthy(xf_Value v) {
    if (v.state != XF_STATE_OK) return false;
    if (v.type == XF_TYPE_NUM) return v.data.num != 0.0;
    if (v.type == XF_TYPE_STR) return v.data.str && v.data.str->len > 0;
    if (v.type == XF_TYPE_ARR) return v.data.arr && v.data.arr->len > 0;
    if (v.type == XF_TYPE_MAP || v.type == XF_TYPE_SET)
        return v.data.map && v.data.map->used > 0;
    /* fn, regex, module — non-null pointer is truthy */
    return true;
}
static xf_Value make_bool(bool b) {
    return xf_val_ok_num(b ? 1.0 : 0.0);
}
static int val_cmp(xf_Value a, xf_Value b) {
    /* numeric path: coerce_num never allocates (no release needed) */
    xf_Value na = xf_coerce_num(a), nb = xf_coerce_num(b);
    if (na.state == XF_STATE_OK && nb.state == XF_STATE_OK) {
        if (na.data.num < nb.data.num) return -1;
        if (na.data.num > nb.data.num) return  1;
        return 0;
    }
    /* string path: coerce_str may allocate a new xf_Str — release after use */
    xf_Value sa = xf_coerce_str(a), sb = xf_coerce_str(b);
    int cmp = 0;
    if (sa.state == XF_STATE_OK && sb.state == XF_STATE_OK && sa.data.str && sb.data.str)
        cmp = strcmp(sa.data.str->data, sb.data.str->data);
    xf_value_release(sa);
    xf_value_release(sb);
    return cmp;
}
static xf_Value val_concat(xf_Value a, xf_Value b) {
    xf_Value sa = xf_coerce_str(a), sb = xf_coerce_str(b);
    if (sa.state != XF_STATE_OK) { xf_value_release(sb); return sa; }
    if (sb.state != XF_STATE_OK) { xf_value_release(sa); return sb; }
    size_t la = sa.data.str ? sa.data.str->len : 0;
    size_t lb = sb.data.str ? sb.data.str->len : 0;
    char *buf = malloc(la + lb + 1);
    if (la) memcpy(buf, sa.data.str->data, la);
    if (lb) memcpy(buf + la, sb.data.str->data, lb);
    buf[la+lb] = '\0';
    xf_Str *s = xf_str_new(buf, la+lb);
    free(buf);
    xf_Value r = xf_val_ok_str(s);
    xf_str_release(s);
    /* release coerced copies — coerce_str retains for STR type, so always release */
    xf_value_release(sa);
    xf_value_release(sb);
    return r;
}
static double scalar_op(double a, double b, int op) {
    switch(op){case 0:return a+b;case 1:return a-b;case 2:return a*b;
    case 3:return b!=0?a/b:0;case 4:return b!=0?fmod(a,b):0;case 5:return pow(a,b);default:return 0;}
}
static xf_Value elem_op(xf_Value a, xf_Value b, int op);
static xf_Value arr_broadcast(xf_Value a, xf_Value b, int op) {
    bool aa=(a.state==XF_STATE_OK&&a.type==XF_TYPE_ARR&&a.data.arr);
    bool ba=(b.state==XF_STATE_OK&&b.type==XF_TYPE_ARR&&b.data.arr);
    if(aa&&ba){size_t len=a.data.arr->len>b.data.arr->len?a.data.arr->len:b.data.arr->len;xf_arr_t*out=xf_arr_new();for(size_t i=0;i<len;i++){xf_Value av=i<a.data.arr->len?a.data.arr->items[i]:xf_val_ok_num(0);xf_Value bv=i<b.data.arr->len?b.data.arr->items[i]:xf_val_ok_num(0);xf_arr_push(out,elem_op(av,bv,op));}xf_Value r=xf_val_ok_arr(out);xf_arr_release(out);return r;}
    if(aa){xf_arr_t*out=xf_arr_new();for(size_t i=0;i<a.data.arr->len;i++)xf_arr_push(out,elem_op(a.data.arr->items[i],b,op));xf_Value r=xf_val_ok_arr(out);xf_arr_release(out);return r;}
    if(ba){xf_arr_t*out=xf_arr_new();for(size_t i=0;i<b.data.arr->len;i++)xf_arr_push(out,elem_op(a,b.data.arr->items[i],op));xf_Value r=xf_val_ok_arr(out);xf_arr_release(out);return r;}
    return xf_val_nav(XF_TYPE_NUM);
}
static xf_Value elem_op(xf_Value a, xf_Value b, int op) {
    if((a.state==XF_STATE_OK&&a.type==XF_TYPE_ARR)||(b.state==XF_STATE_OK&&b.type==XF_TYPE_ARR)) return arr_broadcast(a,b,op);
    xf_Value na=xf_coerce_num(a),nb=xf_coerce_num(b);
    return xf_val_ok_num(scalar_op((na.state==XF_STATE_OK)?na.data.num:0,(nb.state==XF_STATE_OK)?nb.data.num:0,op));
}
static xf_Value mat_mul(xf_Value a, xf_Value b) {
    if(!(a.state==XF_STATE_OK&&a.type==XF_TYPE_ARR&&a.data.arr)) return xf_val_nav(XF_TYPE_ARR);
    if(!(b.state==XF_STATE_OK&&b.type==XF_TYPE_ARR&&b.data.arr)) return xf_val_nav(XF_TYPE_ARR);
    size_t rows=a.data.arr->len;
    size_t k_max=(rows>0&&a.data.arr->items[0].type==XF_TYPE_ARR&&a.data.arr->items[0].data.arr)?a.data.arr->items[0].data.arr->len:1;
    size_t cols=(b.data.arr->len>0&&b.data.arr->items[0].state==XF_STATE_OK&&b.data.arr->items[0].type==XF_TYPE_ARR&&b.data.arr->items[0].data.arr)?b.data.arr->items[0].data.arr->len:1;
    xf_arr_t *result=xf_arr_new();
    for(size_t i=0;i<rows;i++){
        xf_arr_t *row=xf_arr_new();
        for(size_t j=0;j<cols;j++){
            double sum=0;
            for(size_t k=0;k<k_max;k++){
                double aik=0,bkj=0;
                xf_Value ar=a.data.arr->items[i];
                if(ar.state==XF_STATE_OK&&ar.type==XF_TYPE_ARR&&ar.data.arr&&k<ar.data.arr->len){xf_Value n=xf_coerce_num(ar.data.arr->items[k]);if(n.state==XF_STATE_OK)aik=n.data.num;}
                else if(ar.state==XF_STATE_OK&&ar.type==XF_TYPE_NUM&&k==0)aik=ar.data.num;
                if(k<b.data.arr->len){xf_Value br=b.data.arr->items[k];
                    if(br.state==XF_STATE_OK&&br.type==XF_TYPE_ARR&&br.data.arr&&j<br.data.arr->len){xf_Value n=xf_coerce_num(br.data.arr->items[j]);if(n.state==XF_STATE_OK)bkj=n.data.num;}
                    else if(br.state==XF_STATE_OK&&br.type==XF_TYPE_NUM&&j==0)bkj=br.data.num;}
                sum+=aik*bkj;
            }
            xf_arr_push(row,xf_val_ok_num(sum));
        }
        xf_Value rv=xf_val_ok_arr(row);xf_arr_release(row);xf_arr_push(result,rv);
    }
    xf_Value r=xf_val_ok_arr(result);xf_arr_release(result);return r;
}
static xf_Value apply_assign_op(AssignOp op, xf_Value cur, xf_Value rhs) {
    switch (op) {
        case ASSIGNOP_EQ:     return rhs;
        case ASSIGNOP_ADD: if((cur.state==XF_STATE_OK&&cur.type==XF_TYPE_ARR)||(rhs.state==XF_STATE_OK&&rhs.type==XF_TYPE_ARR))return arr_broadcast(cur,rhs,0); {xf_Value a=xf_coerce_num(cur),b=xf_coerce_num(rhs);if(a.state!=XF_STATE_OK)return a;if(b.state!=XF_STATE_OK)return b;return xf_val_ok_num(a.data.num+b.data.num);}
        case ASSIGNOP_SUB: if((cur.state==XF_STATE_OK&&cur.type==XF_TYPE_ARR)||(rhs.state==XF_STATE_OK&&rhs.type==XF_TYPE_ARR))return arr_broadcast(cur,rhs,1); {xf_Value a=xf_coerce_num(cur),b=xf_coerce_num(rhs);if(a.state!=XF_STATE_OK)return a;if(b.state!=XF_STATE_OK)return b;return xf_val_ok_num(a.data.num-b.data.num);}
        case ASSIGNOP_MUL: if((cur.state==XF_STATE_OK&&cur.type==XF_TYPE_ARR)||(rhs.state==XF_STATE_OK&&rhs.type==XF_TYPE_ARR))return arr_broadcast(cur,rhs,2); {xf_Value a=xf_coerce_num(cur),b=xf_coerce_num(rhs);if(a.state!=XF_STATE_OK)return a;if(b.state!=XF_STATE_OK)return b;return xf_val_ok_num(a.data.num*b.data.num);}
        case ASSIGNOP_DIV: if((cur.state==XF_STATE_OK&&cur.type==XF_TYPE_ARR)||(rhs.state==XF_STATE_OK&&rhs.type==XF_TYPE_ARR))return arr_broadcast(cur,rhs,3); {xf_Value a=xf_coerce_num(cur),b=xf_coerce_num(rhs);if(b.state==XF_STATE_OK&&b.data.num==0)return xf_val_err(xf_err_new("division by zero","<interp>",0,0),XF_TYPE_NUM);if(a.state!=XF_STATE_OK)return a;if(b.state!=XF_STATE_OK)return b;return xf_val_ok_num(a.data.num/b.data.num);}
        case ASSIGNOP_MOD: if((cur.state==XF_STATE_OK&&cur.type==XF_TYPE_ARR)||(rhs.state==XF_STATE_OK&&rhs.type==XF_TYPE_ARR))return arr_broadcast(cur,rhs,4); {xf_Value a=xf_coerce_num(cur),b=xf_coerce_num(rhs);if(b.state==XF_STATE_OK&&b.data.num==0)return xf_val_err(xf_err_new("modulo by zero","<interp>",0,0),XF_TYPE_NUM);if(a.state!=XF_STATE_OK)return a;if(b.state!=XF_STATE_OK)return b;return xf_val_ok_num(fmod(a.data.num,b.data.num));}
        case ASSIGNOP_CONCAT: return val_concat(cur, rhs);
        default: return rhs;
    }
}
static xf_Value lvalue_load(Interp *it, Expr *target);
static bool lvalue_store(Interp *it, Expr *target, xf_Value val) {
    if (target->kind == EXPR_IDENT) {
        return sym_assign(it->syms, target->as.ident.name, val);
    }
    if (target->kind == EXPR_IVAR) {
        xf_Value sv = xf_coerce_str(val);
        const char *s = (sv.state == XF_STATE_OK && sv.data.str)
                         ? sv.data.str->data : "";
        VM *vm = it->vm;
        bool _ivar_matched = false;
        switch (target->as.ivar.var) {
            case TK_VAR_FS:  strncpy(IT_REC(it)->fs,  s, sizeof(IT_REC(it)->fs)-1);  _ivar_matched = true; break;
            case TK_VAR_RS:  strncpy(IT_REC(it)->rs,  s, sizeof(IT_REC(it)->rs)-1);  _ivar_matched = true; break;
            case TK_VAR_OFS: strncpy(IT_REC(it)->ofs, s, sizeof(IT_REC(it)->ofs)-1); _ivar_matched = true; break;
            case TK_VAR_ORS: strncpy(IT_REC(it)->ors, s, sizeof(IT_REC(it)->ors)-1); _ivar_matched = true; break;
            default: break;
        }
        xf_value_release(sv);
        if (_ivar_matched) return true;
    }
    if (target->kind == EXPR_SUBSCRIPT) {
        Expr *obj_expr = target->as.subscript.obj;
        xf_Value key   = interp_eval_expr(it, target->as.subscript.key);
        if (key.state != XF_STATE_OK) {
            interp_error(it, target->loc, "subscript key is not OK");
            return false;
        }
        xf_Value container = lvalue_load(it, obj_expr);
        if (container.state == XF_STATE_OK && container.type == XF_TYPE_TUPLE) {
    interp_error(it, target->loc, "cannot assign to tuple element");
    return false;
}
        if (container.state != XF_STATE_OK ||
            (container.type != XF_TYPE_ARR && container.type != XF_TYPE_MAP)) {
            xf_Value num_key = xf_coerce_num(key);
            if (num_key.state == XF_STATE_OK) {
                xf_arr_t *a = xf_arr_new();
                container   = xf_val_ok_arr(a);
                xf_arr_release(a);
            } else {
                xf_map_t *m = xf_map_new();
                container   = xf_val_ok_map(m);
                xf_map_release(m);
            }
            lvalue_store(it, obj_expr, container);
        }
        if (container.type == XF_TYPE_ARR && container.data.arr) {
            xf_Value ni = xf_coerce_num(key);
            if (ni.state != XF_STATE_OK) {
                interp_error(it, target->loc, "array index must be numeric");
                return false;
            }
            xf_arr_set(container.data.arr, (size_t)ni.data.num, val);
            return true;
        }
        if (container.type == XF_TYPE_MAP && container.data.map) {
            xf_Value sk = xf_coerce_str(key);
            if (sk.state != XF_STATE_OK || !sk.data.str) {
                xf_value_release(sk);
                interp_error(it, target->loc, "map key must be a string");
                return false;
            }
            xf_map_set(container.data.map, sk.data.str, val);
            xf_value_release(sk);
            return true;
        }
        interp_error(it, target->loc, "cannot index into type '%s'",
                     XF_TYPE_NAMES[container.type]);
        return false;
    }
    interp_error(it, target->loc, "invalid assignment target");
    return false;
}
static xf_Value lvalue_load(Interp *it, Expr *target) {
    if (target->kind == EXPR_IDENT) {
        Symbol *s = sym_lookup_str(it->syms, target->as.ident.name);
        if (!s) {
            interp_error(it, target->loc, "undefined variable '%s'",
                         target->as.ident.name->data);
            return xf_val_nav(XF_TYPE_VOID);
        }
        return xf_value_retain(s->value);
    }
    return interp_eval_expr(it, target);
}
static size_t xf_sprintf_impl(char *out, size_t cap,
                               const char *fmt,
                               xf_Value *args, size_t argc) {
    size_t wi = 0;
    size_t ai = 0;
#define PUTC(c) do { if (wi < cap-1) out[wi++] = (c); } while(0)
#define PUTS(s) do { const char *_s=(s); while(*_s && wi<cap-1) out[wi++]=*_s++; } while(0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUTC(*p); continue; }
        p++;
        if (!*p) break;
        if (*p == '%') { PUTC('%'); continue; }
        char flags[8] = {0}; int nf = 0;
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#')
            flags[nf++] = *p++;
        char wbuf[16] = {0}; int wn = 0;
        while (isdigit((unsigned char)*p)) wbuf[wn++] = *p++;
        char pbuf[16] = {0};
        if (*p == '.') { p++; int pn=0; while(isdigit((unsigned char)*p)) pbuf[pn++]=*p++; }
        char subfmt[64];
        snprintf(subfmt, sizeof(subfmt), "%%%s%s%s%c",
                 flags, wbuf, *pbuf ? "." : "", *p ? *p : 's');
        if (*pbuf) snprintf(subfmt, sizeof(subfmt), "%%%s%s.%s%c",
                             flags, wbuf, pbuf, *p ? *p : 's');
        xf_Value av = (ai < argc) ? args[ai++] : xf_val_ok_num(0);
        char tmp[512];
        switch (*p) {
            case 'd': case 'i': {
                xf_Value n = xf_coerce_num(av);
                snprintf(tmp, sizeof(tmp), subfmt, (long long)(n.state==XF_STATE_OK?n.data.num:0));
                PUTS(tmp); break;
            }
            case 'u': {
                xf_Value n = xf_coerce_num(av);
                snprintf(tmp, sizeof(tmp), subfmt, (unsigned long long)(n.state==XF_STATE_OK?n.data.num:0));
                PUTS(tmp); break;
            }
            case 'o': case 'x': case 'X': {
                xf_Value n = xf_coerce_num(av);
                snprintf(tmp, sizeof(tmp), subfmt, (unsigned long long)(n.state==XF_STATE_OK?n.data.num:0));
                PUTS(tmp); break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                xf_Value n = xf_coerce_num(av);
                snprintf(tmp, sizeof(tmp), subfmt, n.state==XF_STATE_OK?n.data.num:0.0);
                PUTS(tmp); break;
            }
            case 's': {
                xf_Value s = xf_coerce_str(av);
                snprintf(tmp, sizeof(tmp), subfmt,
                         (s.state==XF_STATE_OK&&s.data.str) ? s.data.str->data : "");
                xf_value_release(s);
                PUTS(tmp); break;
            }
            case 'c': {
                xf_Value n = xf_coerce_num(av);
                char c = (n.state==XF_STATE_OK) ? (char)(int)n.data.num : '?';
                PUTC(c); break;
            }
            default:
                PUTC('%'); PUTC(*p); break;
        }
    }
    out[wi] = '\0';
    return wi;
#undef PUTC
#undef PUTS
}
static void csv_quote(FILE *f, const char *s) {
    bool needs = (strchr(s, ',') || strchr(s, '"') || strchr(s, '\n'));
    if (!needs) { fputs(s, f); return; }
    fputc('"', f);
    for (; *s; s++) { if (*s == '"') fputc('"', f); fputc(*s, f); }
    fputc('"', f);
}
static void tsv_escape(FILE *f, const char *s) {
    for (; *s; s++) {
        if (*s == '\t') fputs("\\t", f);
        else            fputc(*s, f);
    }
}
static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n",  f); break;
            case '\r': fputs("\\r",  f); break;
            case '\t': fputs("\\t",  f); break;
            default:   fputc(*s, f);     break;
        }
    }
    fputc('"', f);
}
static void print_structured(Interp *it, xf_Value *vals, size_t count,
                              uint8_t mode) {
    RecordCtx *_rc = IT_REC(it);
    switch (mode) {
        case XF_OUTFMT_CSV:
            for (size_t i = 0; i < count; i++) {
                if (i > 0) fputc(',', stdout);
                xf_Value sv = xf_coerce_str(vals[i]);
                csv_quote(stdout, (sv.state==XF_STATE_OK&&sv.data.str)
                                  ? sv.data.str->data : "");
                xf_value_release(sv);
            }
            fputs(_rc->ors, stdout);
            break;
        case XF_OUTFMT_TSV:
            for (size_t i = 0; i < count; i++) {
                if (i > 0) fputc('\t', stdout);
                xf_Value sv = xf_coerce_str(vals[i]);
                tsv_escape(stdout, (sv.state==XF_STATE_OK&&sv.data.str)
                                   ? sv.data.str->data : "");
                xf_value_release(sv);
            }
            fputs(_rc->ors, stdout);
            break;
        case XF_OUTFMT_JSON: {
            fputc('{', stdout);
            for (size_t i = 0; i < count; i++) {
                if (i > 0) fputc(',', stdout);
                if (i < _rc->header_count)
                    json_str(stdout, _rc->headers[i]);
                else {
                    char key[32]; snprintf(key, sizeof(key), "f%zu", i+1);
                    json_str(stdout, key);
                }
                fputc(':', stdout);
                xf_Value sv = xf_coerce_str(vals[i]);
                json_str(stdout, (sv.state==XF_STATE_OK&&sv.data.str)
                                 ? sv.data.str->data : "");
                xf_value_release(sv);
            }
            fputs("}", stdout);
            fputs(_rc->ors, stdout);
            break;
        }
        default:
            for (size_t i = 0; i < count; i++) {
                if (i > 0) fputs(_rc->ofs, stdout);
                xf_Value sv = xf_coerce_str(vals[i]);
                if (sv.state==XF_STATE_OK && sv.data.str)
                    fputs(sv.data.str->data, stdout);
                else
                    fputs(XF_STATE_NAMES[vals[i].state], stdout);
                xf_value_release(sv);
            }
            fputs(_rc->ors, stdout);
            break;
    }
}
/* ── builtin dispatch table ──────────────────────────────────────────
 * Maps bare function name → (module, fn_name) for core delegation.
 * Sorted by name so future binary search is possible; currently hashed
 * via FNV-1a for O(1) average dispatch without a giant strcmp chain.
 * ─────────────────────────────────────────────────────────────────── */
typedef struct { const char *name; const char *mod; const char *fn; } CoreRoute;

static const CoreRoute k_core_routes[] = {
    /* math */
    { "abs",   "math", "abs"   }, { "cos",   "math", "cos"   },
    { "int",   "math", "int"   }, { "rand",  "math", "rand"  },
    { "sin",   "math", "sin"   }, { "sqrt",  "math", "sqrt"  },
    { "srand", "math", "srand" },
    /* string */
    { "column",  "str", "column"      }, { "gsub",    "str", "replace_all" },
    { "index",   "str", "index"       }, { "len",     "str", "len"         },
    { "lower",   "str", "lower"       }, { "sprintf", "str", "sprintf"     },
    { "sub",     "str", "replace"     }, { "substr",  "str", "substr"      },
    { "tolower", "str", "lower"       }, { "toupper", "str", "upper"       },
    { "trim",    "str", "trim"        }, { "upper",   "str", "upper"       },
};
#define CORE_ROUTE_COUNT (sizeof(k_core_routes)/sizeof(k_core_routes[0]))

static int core_route_cmp(const void *key, const void *elem) {
    return strcmp((const char *)key, ((const CoreRoute *)elem)->name);
}

/* Lookup name in the sorted route table; returns NULL if not found. */
static const CoreRoute *find_core_route(const char *name) {
    return (const CoreRoute *)bsearch(name, k_core_routes, CORE_ROUTE_COUNT,
                                      sizeof(CoreRoute), core_route_cmp);
}

xf_Value interp_call_builtin(Interp *it, const char *name,
                              xf_Value *args, size_t argc) {
    /* ── core module delegation (math + string) ──────────────── */
    const CoreRoute *rt = find_core_route(name);
    if (rt) return interp_call_core(it, rt->mod, rt->fn, args, argc);
    if (strcmp(name,"system")==0 && argc==1) {
        xf_Value s = xf_coerce_str(args[0]);
        if (s.state != XF_STATE_OK) return s;
        int rc = system(s.data.str->data);
        xf_value_release(s);
        return xf_val_ok_num((double)rc);
    }
    if (strcmp(name,"exit")==0) {
        int code = (argc>0) ? (int)xf_coerce_num(args[0]).data.num : 0;
        exit(code);
    }
    if (strcmp(name,"push")==0 && argc==2) {
        if (args[0].state==XF_STATE_OK&&args[0].type==XF_TYPE_ARR&&args[0].data.arr)
            xf_arr_push(args[0].data.arr, args[1]);
        else if (args[0].state==XF_STATE_OK&&args[0].type==XF_TYPE_SET&&args[0].data.map) {
            xf_Value ks=xf_coerce_str(args[1]);
            if (ks.state==XF_STATE_OK&&ks.data.str)
                xf_map_set(args[0].data.map, ks.data.str, xf_val_ok_num(1.0));
            xf_value_release(ks);
        }
        return args[0];
    }
    if (strcmp(name,"pop")==0 && argc==1) {
        if (args[0].state==XF_STATE_OK&&args[0].type==XF_TYPE_ARR&&args[0].data.arr)
            return xf_arr_pop(args[0].data.arr);
        return xf_val_nav(XF_TYPE_VOID);
    }
    if (strcmp(name,"shift")==0 && argc==1) {
        if (args[0].state==XF_STATE_OK&&args[0].type==XF_TYPE_ARR&&args[0].data.arr)
            return xf_arr_shift(args[0].data.arr);
        return xf_val_nav(XF_TYPE_VOID);
    }
    if (strcmp(name,"unshift")==0 && argc==2) {
        if (args[0].state==XF_STATE_OK&&args[0].type==XF_TYPE_ARR&&args[0].data.arr)
            xf_arr_unshift(args[0].data.arr, args[1]);
        return args[0];
    }
    if (strcmp(name,"remove")==0 && argc==2) {
        xf_Value coll=args[0];
        if (coll.state!=XF_STATE_OK) return xf_val_nav(XF_TYPE_VOID);
        if (coll.type==XF_TYPE_ARR&&coll.data.arr) {
            xf_Value ni=xf_coerce_num(args[1]);
            if (ni.state==XF_STATE_OK) xf_arr_delete(coll.data.arr,(size_t)ni.data.num);
            return coll;
        }
        if ((coll.type==XF_TYPE_MAP||coll.type==XF_TYPE_SET)&&coll.data.map) {
            xf_Value ks=xf_coerce_str(args[1]);
            if (ks.state==XF_STATE_OK&&ks.data.str) xf_map_delete(coll.data.map,ks.data.str);
            xf_value_release(ks);
            return coll;
        }
        return xf_val_nav(XF_TYPE_VOID);
    }
    if (strcmp(name,"has")==0 && argc==2) {
        xf_Value coll=args[0];
        if (coll.state!=XF_STATE_OK||!coll.data.map) return xf_val_ok_num(0);
        if (coll.type==XF_TYPE_MAP||coll.type==XF_TYPE_SET) {
            xf_Value ks=xf_coerce_str(args[1]);
            if (ks.state!=XF_STATE_OK||!ks.data.str) { xf_value_release(ks); return xf_val_ok_num(0); }
            double _has = xf_map_get(coll.data.map,ks.data.str).state!=XF_STATE_NAV ? 1.0 : 0.0;
            xf_value_release(ks);
            return xf_val_ok_num(_has);
        }
        return xf_val_ok_num(0);
    }
    if (strcmp(name,"keys")==0 && argc==1) {
        xf_Value coll=args[0];
        if (coll.state!=XF_STATE_OK||!coll.data.map) return xf_val_nav(XF_TYPE_ARR);
        xf_arr_t *a=xf_arr_new();
        xf_map_t *m=coll.data.map;
        for (size_t i=0;i<m->order_len;i++) xf_arr_push(a,xf_val_ok_str(m->order[i]));
        xf_Value r=xf_val_ok_arr(a); xf_arr_release(a); return r;
    }
    if (strcmp(name,"values")==0 && argc==1) {
        xf_Value coll=args[0];
        if (coll.state!=XF_STATE_OK||!coll.data.map) return xf_val_nav(XF_TYPE_ARR);
        xf_arr_t *a=xf_arr_new();
        xf_map_t *m=coll.data.map;
        for (size_t i=0;i<m->order_len;i++) {
            xf_Value v=xf_map_get(m,m->order[i]);
            xf_arr_push(a,v);
        }
        xf_Value r=xf_val_ok_arr(a); xf_arr_release(a); return r;
    }
    if (strcmp(name,"read")==0 && argc==1) {
        xf_Value sv = xf_coerce_str(args[0]);
        if (sv.state != XF_STATE_OK || !sv.data.str) { xf_value_release(sv); return xf_val_nav(XF_TYPE_STR); }
        bool is_pipe = true;
        FILE *fp = popen(sv.data.str->data, "r");
        if (!fp) { is_pipe = false; fp = fopen(sv.data.str->data, "r"); }
        if (!fp) { xf_value_release(sv); return xf_val_nav(XF_TYPE_STR); }
        char buf[65536];
        size_t n = 0;
        int c;
        while (n < sizeof(buf) - 1 && (c = fgetc(fp)) != EOF) {
            buf[n++] = (char)c;
        }
        buf[n] = '\0';
        if (is_pipe) pclose(fp); else fclose(fp);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
        xf_Str *r = xf_str_from_cstr(buf);
        xf_Value rv = xf_val_ok_str(r); xf_str_release(r);
        xf_value_release(sv);
        return rv;
    }
    if (strcmp(name,"lines")==0 && argc==1) {
        xf_Value sv = xf_coerce_str(args[0]);
        if (sv.state != XF_STATE_OK || !sv.data.str) { xf_value_release(sv); return xf_val_nav(XF_TYPE_ARR); }
        bool is_pipe = true;
        FILE *fp = popen(sv.data.str->data, "r");
        if (!fp) { is_pipe = false; fp = fopen(sv.data.str->data, "r"); }
        if (!fp) { xf_value_release(sv); return xf_val_nav(XF_TYPE_ARR); }
        xf_arr_t *a = xf_arr_new();
        char line[4096];
        while (fgets(line, sizeof(line), fp)) {
            size_t ln = strlen(line);
            while (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) {
                line[--ln] = '\0';
            }
            xf_Str *ls = xf_str_new(line, ln);
            xf_Value lv = xf_val_ok_str(ls);
            xf_str_release(ls);
            xf_arr_push(a, lv);
        }
        if (is_pipe) pclose(fp);
        else fclose(fp);
        xf_Value r = xf_val_ok_arr(a);
        xf_arr_release(a);
        return r;
    }
    if (strcmp(name,"write")==0 && argc==2) {
        xf_Value fv=xf_coerce_str(args[0]),sv=xf_coerce_str(args[1]);
        if (fv.state!=XF_STATE_OK||sv.state!=XF_STATE_OK) { xf_value_release(fv); xf_value_release(sv); return xf_val_ok_num(0); }
        FILE *fp=fopen(fv.data.str->data,"w");
        if(!fp) { xf_value_release(fv); xf_value_release(sv); return xf_val_ok_num(0); }
        fwrite(sv.data.str->data,1,sv.data.str->len,fp); fclose(fp);
        xf_value_release(fv); xf_value_release(sv);
        return xf_val_ok_num(1);
    }
    if (strcmp(name,"append")==0 && argc==2) {
        xf_Value fv=xf_coerce_str(args[0]),sv=xf_coerce_str(args[1]);
        if (fv.state!=XF_STATE_OK||sv.state!=XF_STATE_OK) { xf_value_release(fv); xf_value_release(sv); return xf_val_ok_num(0); }
        FILE *fp=fopen(fv.data.str->data,"a");
        if(!fp) { xf_value_release(fv); xf_value_release(sv); return xf_val_ok_num(0); }
        fwrite(sv.data.str->data,1,sv.data.str->len,fp); fclose(fp);
        xf_value_release(fv); xf_value_release(sv);
        return xf_val_ok_num(1);
    }
    /* close(path) — flush and evict one handle from the redirect cache */
    if (strcmp(name,"close")==0 && argc==1) {
        xf_Value sv = xf_coerce_str(args[0]);
        if (sv.state==XF_STATE_OK && sv.data.str && it) {
            VM *_vm = it->vm;
            for (size_t _i=0; _i<_vm->redir_count; _i++) {
                if (strcmp(_vm->redir[_i].path, sv.data.str->data)==0) {
                    if (_vm->redir[_i].fp) {
                        fflush(_vm->redir[_i].fp);
                        if (_vm->redir[_i].is_pipe) pclose(_vm->redir[_i].fp);
                        else                         fclose(_vm->redir[_i].fp);
                        _vm->redir[_i].fp = NULL;
                    }
                    /* compact the array */
                    _vm->redir[_i] = _vm->redir[--_vm->redir_count];
                    break;
                }
            }
        }
        xf_value_release(sv);
        return xf_val_ok_num(0);
    }
    /* flush([path]) — fflush one handle or all */
    if (strcmp(name,"flush")==0) {
        if (argc==0 || (argc==1 && args[0].state!=XF_STATE_OK)) {
            fflush(stdout);
            if (it) { VM *_vm=it->vm; for(size_t _i=0;_i<_vm->redir_count;_i++) if(_vm->redir[_i].fp) fflush(_vm->redir[_i].fp); }
        } else if (argc==1) {
            xf_Value sv=xf_coerce_str(args[0]);
            if (sv.state==XF_STATE_OK && sv.data.str && it) {
                VM *_vm=it->vm;
                for(size_t _i=0;_i<_vm->redir_count;_i++)
                    if(strcmp(_vm->redir[_i].path, sv.data.str->data)==0 && _vm->redir[_i].fp)
                        { fflush(_vm->redir[_i].fp); break; }
            }
            xf_value_release(sv);
        }
        return xf_val_ok_num(0);
    }
    return xf_val_nav(XF_TYPE_VOID);
}
xf_Value interp_eval_expr(Interp *it, Expr *e) {
    if (!e) return xf_val_null();
    if (it->had_error || it->returning || it->exiting || it->nexting)
        return xf_val_null();
    switch (e->kind) {
    case EXPR_NUM:
        return xf_val_ok_num(e->as.num);
    case EXPR_STR: {
        xf_Value v = xf_val_ok_str(e->as.str.value);
        return v;
    }
    case EXPR_REGEX: {
        /* Compile the regex literal into an xf_Regex value */
        xf_Regex *re = calloc(1, sizeof(xf_Regex));
        if (!re) return xf_val_nav(XF_TYPE_REGEX);
        atomic_init(&re->refcount, 1);
        re->pattern  = xf_str_retain(e->as.regex.pattern);
        re->flags    = e->as.regex.flags;   /* XF_RE_* flags */
        re->compiled = NULL;                /* lazy compile on first use */
        return xf_val_ok_re(re);
    }
case EXPR_TUPLE_LIT: {
    size_t n = e->as.tuple_lit.count;
    xf_Value *items = n ? malloc(sizeof(xf_Value) * n) : NULL;
    for (size_t i = 0; i < n; i++) {
        xf_Value v = interp_eval_expr(it, e->as.tuple_lit.items[i]);
        if (v.state != XF_STATE_OK) {
            for (size_t j = 0; j < i; j++) xf_value_release(items[j]);
            free(items);
            return v;
        }
        items[i] = v;
    }
    xf_tuple_t *t = xf_tuple_new(items, n);
    free(items);
    xf_Value out = xf_val_ok_tuple(t);
    xf_tuple_release(t);
    return out;
}
        case EXPR_ARR_LIT: {
        xf_arr_t *a = xf_arr_new();
        for (size_t i = 0; i < e->as.arr_lit.count; i++) {
            xf_Value v = interp_eval_expr(it, e->as.arr_lit.items[i]);
            if (v.state != XF_STATE_OK) {
                xf_arr_release(a);
                return v;
            }
            xf_arr_push(a, v);
        }
        xf_Value out = xf_val_ok_arr(a);
        xf_arr_release(a);
        return out;
    }
    case EXPR_MAP_LIT: {
        xf_map_t *m=xf_map_new();
        for(size_t i=0;i<e->as.map_lit.count;i++){
            xf_Value kv=interp_eval_expr(it,e->as.map_lit.keys[i]); if(kv.state!=XF_STATE_OK){xf_map_release(m);return kv;}
            xf_Value vv=interp_eval_expr(it,e->as.map_lit.vals[i]); if(vv.state!=XF_STATE_OK){xf_map_release(m);return vv;}
            xf_Value ks=xf_coerce_str(kv); if(ks.state!=XF_STATE_OK){xf_map_release(m);return ks;}
            xf_map_set(m,ks.data.str,vv); xf_str_release(ks.data.str);
        }
xf_Value out = xf_val_ok_map(m);
xf_map_release(m);
return out;
        }
    case EXPR_SET_LIT: {
        xf_map_t *m=xf_map_new();
        for(size_t i=0;i<e->as.set_lit.count;i++){
            xf_Value v=interp_eval_expr(it,e->as.set_lit.items[i]); if(v.state!=XF_STATE_OK){xf_map_release(m);return v;}
            if(v.state==XF_STATE_OK&&(v.type==XF_TYPE_SET||v.type==XF_TYPE_MAP||v.type==XF_TYPE_ARR)){
                char kb[32]; snprintf(kb,sizeof(kb),"#%zu",i);
                xf_Str *ks=xf_str_from_cstr(kb); xf_map_set(m,ks,v); xf_str_release(ks);
            } else {
                xf_Value ks=xf_coerce_str(v); if(ks.state!=XF_STATE_OK){xf_map_release(m);return ks;}
                xf_map_set(m,ks.data.str,xf_val_ok_num(1.0)); xf_str_release(ks.data.str);
            }
        }
xf_Value out = xf_val_ok_map(m);
xf_map_release(m);
out.type = XF_TYPE_SET;
return out;
        }
    case EXPR_IDENT: {
        Symbol *s = sym_lookup_str(it->syms, e->as.ident.name);
        if (!s) {
            interp_error(it, e->loc, "undefined variable '%s'",
                         e->as.ident.name->data);
            return xf_val_nav(XF_TYPE_VOID);
        }
        return xf_value_retain(s->value);  /* return owned ref — caller steals */
    }
    case EXPR_FIELD: {
        int n = e->as.field.index;
        RecordCtx *_rc = IT_REC(it);
        if (n == 0) {
            if (!_rc->buf) return xf_val_ok_str(xf_str_from_cstr(""));
            xf_Str *s = xf_str_new(_rc->buf, _rc->buf_len);
            xf_Value v = xf_val_ok_str(s); xf_str_release(s); return v;
        }
        if (n > 0 && (size_t)n <= _rc->field_count) {
            xf_Str *s = xf_str_from_cstr(_rc->fields[n-1]);
            xf_Value v = xf_val_ok_str(s); xf_str_release(s); return v;
        }
        return xf_val_ok_str(xf_str_from_cstr(""));
    }
    case EXPR_IVAR: {
        RecordCtx *_rc = IT_REC(it);
        switch (e->as.ivar.var) {
            case TK_VAR_NR:  return xf_val_ok_num((double)_rc->nr);
            case TK_VAR_NF:  return xf_val_ok_num((double)_rc->field_count);
            case TK_VAR_FNR: return xf_val_ok_num((double)_rc->fnr);
            case TK_VAR_FS:  { xf_Str *s=xf_str_from_cstr(_rc->fs);  xf_Value v=xf_val_ok_str(s);xf_str_release(s);return v; }
            case TK_VAR_RS:  { xf_Str *s=xf_str_from_cstr(_rc->rs);  xf_Value v=xf_val_ok_str(s);xf_str_release(s);return v; }
            case TK_VAR_OFS: { xf_Str *s=xf_str_from_cstr(_rc->ofs); xf_Value v=xf_val_ok_str(s);xf_str_release(s);return v; }
            case TK_VAR_ORS: { xf_Str *s=xf_str_from_cstr(_rc->ors); xf_Value v=xf_val_ok_str(s);xf_str_release(s);return v; }
            default: return xf_val_nav(XF_TYPE_VOID);
        }
    }
case EXPR_SUBSCRIPT: {
    xf_Value obj = interp_eval_expr(it, e->as.subscript.obj);
    xf_Value key = interp_eval_expr(it, e->as.subscript.key);

    if (obj.state != XF_STATE_OK) {
        xf_value_release(key);
        return obj;
    }
    if (key.state != XF_STATE_OK) {
        xf_value_release(obj);
        return key;
    }

    xf_Value _sub_result = xf_val_nav(XF_TYPE_VOID);

    if (obj.type == XF_TYPE_ARR) {
        xf_Value ni = xf_coerce_num(key);
        if (ni.state == XF_STATE_OK && obj.data.arr) {
            if (ni.data.num < 0) {
                interp_error(it, e->loc, "negative index");
                _sub_result = xf_val_nav(XF_TYPE_VOID);
            } else {
                size_t idx = (size_t)ni.data.num;
                if (idx >= obj.data.arr->len) {
                    interp_error(it, e->loc, "array index out of range");
                    _sub_result = xf_val_nav(XF_TYPE_VOID);
                } else {
                    _sub_result = xf_value_retain(
                        xf_arr_get(obj.data.arr, idx)
                    );
                }
            }
        }

    } else if (obj.type == XF_TYPE_TUPLE) {
        xf_Value ni = xf_coerce_num(key);
        if (ni.state == XF_STATE_OK) {
            if (ni.data.num < 0) {
                interp_error(it, e->loc, "negative index");
                _sub_result = xf_val_nav(XF_TYPE_VOID);
            } else {
                size_t idx = (size_t)ni.data.num;
                if (!obj.data.tuple || idx >= xf_tuple_len(obj.data.tuple)) {
                    interp_error(it, e->loc, "tuple index out of range");
                    _sub_result = xf_val_nav(XF_TYPE_VOID);
                } else {
                    _sub_result = xf_value_retain(
                        xf_tuple_get(obj.data.tuple, idx)
                    );
                }
            }
        }

    } else if (obj.type == XF_TYPE_MAP) {
        xf_Value sk = xf_coerce_str(key);
        if (sk.state == XF_STATE_OK && sk.data.str && obj.data.map) {
            _sub_result = xf_value_retain(
                xf_map_get(obj.data.map, sk.data.str)
            );
        }
        xf_value_release(sk);

    } else if (obj.type == XF_TYPE_STR && obj.data.str) {
        xf_Value ni = xf_coerce_num(key);
        if (ni.state == XF_STATE_OK) {
            if (ni.data.num < 0) {
                interp_error(it, e->loc, "negative index");
                _sub_result = xf_val_nav(XF_TYPE_STR);
            } else {
                size_t idx = (size_t)ni.data.num;
                if (idx < obj.data.str->len) {
                    char ch[2] = { obj.data.str->data[idx], '\0' };
                    xf_Str *cs = xf_str_new(ch, 1);
                    _sub_result = xf_val_ok_str(cs);
                    xf_str_release(cs);
                } else {
                    _sub_result = xf_val_nav(XF_TYPE_STR);
                }
            }
        } else {
            _sub_result = xf_val_nav(XF_TYPE_STR);
        }

    } else {
        interp_error(it, e->loc, "subscript on non-indexable type '%s'",
                     XF_TYPE_NAMES[obj.type]);
    }

    xf_value_release(obj);
    xf_value_release(key);
    return _sub_result;
}
        case EXPR_SVAR: {
        RecordCtx *_rc = IT_REC(it);
        switch (e->as.svar.var) {
            case TK_VAR_FILE: {
                xf_Str *s = xf_str_from_cstr(_rc->current_file);
                xf_Value v = xf_val_ok_str(s); xf_str_release(s); return v;
            }
            case TK_VAR_MATCH:
                return xf_value_retain(_rc->last_match);
            case TK_VAR_CAPS:
                return xf_value_retain(_rc->last_captures);
            case TK_VAR_ERR:
                return xf_value_retain(it->last_err);
            default: {
                xf_Str *s = xf_str_from_cstr("");
                xf_Value v = xf_val_ok_str(s); xf_str_release(s); return v;
            }
        }
    }
    case EXPR_UNARY: {
        switch (e->as.unary.op) {
            case UNOP_NEG: {
                xf_Value v = xf_coerce_num(interp_eval_expr(it, e->as.unary.operand));
                if (v.state != XF_STATE_OK) return v;
                return xf_val_ok_num(-v.data.num);
            }
            case UNOP_NOT:
                return make_bool(!is_truthy(interp_eval_expr(it, e->as.unary.operand)));
            case UNOP_PRE_INC: {
                xf_Value v = xf_coerce_num(lvalue_load(it, e->as.unary.operand));
                if (v.state != XF_STATE_OK) return v;
                xf_Value nv = xf_val_ok_num(v.data.num + 1);
                lvalue_store(it, e->as.unary.operand, nv);
                return nv;
            }
            case UNOP_PRE_DEC: {
                xf_Value v = xf_coerce_num(lvalue_load(it, e->as.unary.operand));
                if (v.state != XF_STATE_OK) return v;
                xf_Value nv = xf_val_ok_num(v.data.num - 1);
                lvalue_store(it, e->as.unary.operand, nv);
                return nv;
            }
            case UNOP_POST_INC: {
                xf_Value old = xf_coerce_num(lvalue_load(it, e->as.unary.operand));
                if (old.state == XF_STATE_OK)
                    lvalue_store(it, e->as.unary.operand, xf_val_ok_num(old.data.num+1));
                return old;
            }
            case UNOP_POST_DEC: {
                xf_Value old = xf_coerce_num(lvalue_load(it, e->as.unary.operand));
                if (old.state == XF_STATE_OK)
                    lvalue_store(it, e->as.unary.operand, xf_val_ok_num(old.data.num-1));
                return old;
            }
            default: return xf_val_nav(XF_TYPE_VOID);
        }
    }
    case EXPR_BINARY: {
        if (e->as.binary.op == BINOP_AND) {
            xf_Value a = interp_eval_expr(it, e->as.binary.left);
            bool _ta = is_truthy(a); xf_value_release(a);
            if (!_ta) return make_bool(false);
            xf_Value b = interp_eval_expr(it, e->as.binary.right);
            bool _tb = is_truthy(b); xf_value_release(b);
            return make_bool(_tb);
        }
        if (e->as.binary.op == BINOP_OR) {
            xf_Value a = interp_eval_expr(it, e->as.binary.left);
            bool _ta = is_truthy(a); xf_value_release(a);
            if (_ta) return make_bool(true);
            xf_Value b = interp_eval_expr(it, e->as.binary.right);
            bool _tb = is_truthy(b); xf_value_release(b);
            return make_bool(_tb);
        }
        xf_Value a = interp_eval_expr(it, e->as.binary.left);
        xf_Value b = interp_eval_expr(it, e->as.binary.right);
        switch (e->as.binary.op) {
            case BINOP_ADD:{if((a.state==XF_STATE_OK&&a.type==XF_TYPE_ARR)||(b.state==XF_STATE_OK&&b.type==XF_TYPE_ARR))return arr_broadcast(a,b,0);xf_Value na=xf_coerce_num(a),nb=xf_coerce_num(b);if(na.state!=XF_STATE_OK)return na;if(nb.state!=XF_STATE_OK)return nb;return xf_val_ok_num(na.data.num+nb.data.num);}
            case BINOP_SUB:{if((a.state==XF_STATE_OK&&a.type==XF_TYPE_ARR)||(b.state==XF_STATE_OK&&b.type==XF_TYPE_ARR))return arr_broadcast(a,b,1);xf_Value na=xf_coerce_num(a),nb=xf_coerce_num(b);if(na.state!=XF_STATE_OK)return na;if(nb.state!=XF_STATE_OK)return nb;return xf_val_ok_num(na.data.num-nb.data.num);}
            case BINOP_MUL:{if((a.state==XF_STATE_OK&&a.type==XF_TYPE_ARR)||(b.state==XF_STATE_OK&&b.type==XF_TYPE_ARR))return arr_broadcast(a,b,2);xf_Value na=xf_coerce_num(a),nb=xf_coerce_num(b);if(na.state!=XF_STATE_OK)return na;if(nb.state!=XF_STATE_OK)return nb;return xf_val_ok_num(na.data.num*nb.data.num);}
            case BINOP_DIV:{if((a.state==XF_STATE_OK&&a.type==XF_TYPE_ARR)||(b.state==XF_STATE_OK&&b.type==XF_TYPE_ARR))return arr_broadcast(a,b,3);xf_Value na=xf_coerce_num(a),nb=xf_coerce_num(b);if(nb.state==XF_STATE_OK&&nb.data.num==0)return xf_val_err(xf_err_new("division by zero",e->loc.source,e->loc.line,e->loc.col),XF_TYPE_NUM);if(na.state!=XF_STATE_OK)return na;if(nb.state!=XF_STATE_OK)return nb;return xf_val_ok_num(na.data.num/nb.data.num);}
            case BINOP_MOD:{if((a.state==XF_STATE_OK&&a.type==XF_TYPE_ARR)||(b.state==XF_STATE_OK&&b.type==XF_TYPE_ARR))return arr_broadcast(a,b,4);xf_Value na=xf_coerce_num(a),nb=xf_coerce_num(b);if(nb.state==XF_STATE_OK&&nb.data.num==0)return xf_val_err(xf_err_new("modulo by zero",e->loc.source,e->loc.line,e->loc.col),XF_TYPE_NUM);if(na.state!=XF_STATE_OK)return na;if(nb.state!=XF_STATE_OK)return nb;return xf_val_ok_num(fmod(na.data.num,nb.data.num));}
            case BINOP_POW:{if((a.state==XF_STATE_OK&&a.type==XF_TYPE_ARR)||(b.state==XF_STATE_OK&&b.type==XF_TYPE_ARR))return arr_broadcast(a,b,5);xf_Value na=xf_coerce_num(a),nb=xf_coerce_num(b);if(na.state!=XF_STATE_OK)return na;if(nb.state!=XF_STATE_OK)return nb;return xf_val_ok_num(pow(na.data.num,nb.data.num));}
            case BINOP_MADD: return arr_broadcast(a, b, 0);
            case BINOP_MSUB: return arr_broadcast(a, b, 1);
            case BINOP_MMUL: return mat_mul(a, b);
            case BINOP_MDIV: return arr_broadcast(a, b, 3);
case BINOP_PIPE_CMD: {
    xf_Value cmd = xf_coerce_str(b);
    if (cmd.state != XF_STATE_OK || !cmd.data.str) { xf_value_release(cmd); return xf_val_nav(XF_TYPE_STR); }
    xf_Value sv = xf_coerce_str(a);
    if (sv.state != XF_STATE_OK || !sv.data.str) { xf_value_release(cmd); xf_value_release(sv); return xf_val_nav(XF_TYPE_STR); }

    char tmpname[] = "/tmp/xf_pipe_XXXXXX";
    int fd = mkstemp(tmpname);
    if (fd < 0) { xf_value_release(cmd); xf_value_release(sv); return xf_val_nav(XF_TYPE_STR); }
    FILE *tf = fdopen(fd, "w");
    if (!tf) { close(fd); unlink(tmpname); xf_value_release(cmd); xf_value_release(sv); return xf_val_nav(XF_TYPE_STR); }

    fwrite(sv.data.str->data, 1, sv.data.str->len, tf);
    fclose(tf); /* important: flush + close so child sees EOF */

    char shellcmd[8192];
    snprintf(shellcmd, sizeof(shellcmd), "%s < '%s'", cmd.data.str->data, tmpname);

    FILE *fp = popen(shellcmd, "r");
    if (!fp) { unlink(tmpname); xf_value_release(cmd); xf_value_release(sv); return xf_val_nav(XF_TYPE_STR); }

    char buf[65536];
    size_t n = 0;
    int c;
    while (n < sizeof(buf) - 1 && (c = fgetc(fp)) != EOF)
        buf[n++] = (char)c;
    buf[n] = '\0';

    pclose(fp);
    unlink(tmpname);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    xf_Str *r = xf_str_from_cstr(buf);
    xf_Value rv = xf_val_ok_str(r); xf_str_release(r);
    xf_value_release(cmd); xf_value_release(sv);
    return rv;
}
            case BINOP_EQ:        return make_bool(val_cmp(a,b)==0);
            case BINOP_NEQ:       return make_bool(val_cmp(a,b)!=0);
            case BINOP_LT:        return make_bool(val_cmp(a,b)<0);
            case BINOP_GT:        return make_bool(val_cmp(a,b)>0);
            case BINOP_LTE:       return make_bool(val_cmp(a,b)<=0);
            case BINOP_GTE:       return make_bool(val_cmp(a,b)>=0);
            case BINOP_SPACESHIP: return xf_val_ok_num((double)val_cmp(a,b));
            case BINOP_CONCAT:    return val_concat(a,b);
            case BINOP_MATCH:
            case BINOP_NMATCH: {
    /* LHS: subject string. RHS: compiled xf_Regex or coerced str pattern. */
    bool _is_nmatch = (e->as.binary.op == BINOP_NMATCH);
    xf_Value sa = xf_coerce_str(a);
    if (sa.state != XF_STATE_OK || !sa.data.str) {
        xf_value_release(sa);
        return make_bool(_is_nmatch);
    }
    const char *subject = sa.data.str->data;
    const char *pattern = NULL;
    int cflags = REG_EXTENDED;

    /* track whether we coerced RHS so we can release it */
    xf_Value sb_coerced = xf_val_null();
    bool sb_allocated = false;

    /* use pre-compiled handle if available, avoiding regcomp per-call */
    regex_t *_precompiled = NULL;
    if (b.state == XF_STATE_OK && b.type == XF_TYPE_REGEX && b.data.re) {
        if (b.data.re->compiled) {
            _precompiled = (regex_t *)b.data.re->compiled;
        } else {
            pattern = b.data.re->pattern ? b.data.re->pattern->data : "";
            if (b.data.re->flags & XF_RE_ICASE)    cflags |= REG_ICASE;
            if (b.data.re->flags & XF_RE_MULTILINE) cflags |= REG_NEWLINE;
        }
    } else {
        sb_coerced = xf_coerce_str(b);
        sb_allocated = true;
        if (sb_coerced.state != XF_STATE_OK || !sb_coerced.data.str) {
            xf_value_release(sa);
            xf_value_release(sb_coerced);
            return make_bool(_is_nmatch);
        }
        pattern = sb_coerced.data.str->data;
    }

    regex_t re_local;
    char errbuf[128];
    bool matched;
    regmatch_t _pm[33] = {0};
    int _ngroups = 33;
    if (_precompiled) {
        matched = (regexec(_precompiled, subject, _ngroups, _pm, 0) == 0);
    } else {
        int rcc = regcomp(&re_local, pattern ? pattern : "", cflags);
        if (rcc != 0) {
            regerror(rcc, &re_local, errbuf, sizeof(errbuf));
            interp_error(it, e->loc, "invalid regex '%s': %s",
                         pattern ? pattern : "", errbuf);
            matched = false;
        } else {
            matched = (regexec(&re_local, subject, _ngroups, _pm, 0) == 0);
            regfree(&re_local);
        }
    }

    if (matched) {
        /* populate $match — full matched substring */
        regoff_t _ms = _pm[0].rm_so, _me = _pm[0].rm_eo;
        if (_ms >= 0) {
            xf_Str *_ms_str = xf_str_new(subject + _ms, (size_t)(_me - _ms));
            xf_value_release(IT_REC(it)->last_match);
            IT_REC(it)->last_match = xf_val_ok_str(_ms_str);
            xf_str_release(_ms_str);
        }
        /* populate $captures — arr of subgroup strings */
        xf_arr_t *_caps = xf_arr_new();
        for (int _gi = 1; _gi < _ngroups && _pm[_gi].rm_so >= 0; _gi++) {
            regoff_t _gs = _pm[_gi].rm_so, _ge = _pm[_gi].rm_eo;
            xf_Str *_gs_str = xf_str_new(subject + _gs, (size_t)(_ge - _gs));
            xf_Value _gv = xf_val_ok_str(_gs_str); xf_str_release(_gs_str);
            xf_arr_push(_caps, _gv);
        }
        xf_value_release(IT_REC(it)->last_captures);
        IT_REC(it)->last_captures = xf_val_ok_arr(_caps);
        xf_arr_release(_caps);
    }

    xf_value_release(sa);
    if (sb_allocated) xf_value_release(sb_coerced);
    return make_bool(_is_nmatch ? !matched : matched);
}

            default: return xf_val_nav(XF_TYPE_VOID);
        }
    }
    case EXPR_TERNARY: {
        xf_Value cond = interp_eval_expr(it, e->as.ternary.cond);
        bool _cond_truthy = is_truthy(cond);
        xf_value_release(cond);
        return _cond_truthy
               ? interp_eval_expr(it, e->as.ternary.then)
               : interp_eval_expr(it, e->as.ternary.els);
    }
    case EXPR_COALESCE: {
        xf_Value v = interp_eval_expr(it, e->as.coalesce.left);
        if (v.state == XF_STATE_OK) return v;
        return interp_eval_expr(it, e->as.coalesce.right);
    }
    case EXPR_ASSIGN: {
        xf_Value rhs = interp_eval_expr(it, e->as.assign.value);
        if (e->as.assign.op != ASSIGNOP_EQ) {
            xf_Value cur = lvalue_load(it, e->as.assign.target);
            rhs = apply_assign_op(e->as.assign.op, cur, rhs);
        }
        lvalue_store(it, e->as.assign.target, rhs);
        return rhs;
    }
    case EXPR_WALRUS: {
        xf_Value rhs = interp_eval_expr(it, e->as.walrus.value);
        Symbol *s = sym_declare(it->syms, e->as.walrus.name,
                                SYM_VAR, e->as.walrus.type, e->loc);
        if (s) { xf_value_release(s->value); s->value = rhs; s->state = rhs.state; s->is_defined = true; }
        return rhs;
    }
    case EXPR_CALL: {
        xf_Value args[64];
        size_t argc = e->as.call.argc < 64 ? e->as.call.argc : 64;

        /* evaluate args once */
        for (size_t i = 0; i < argc; i++)
            args[i] = interp_eval_expr(it, e->as.call.args[i]);

        /* method call: obj.method(...) */
        if (e->as.call.callee->kind == EXPR_MEMBER) {
            xf_Value obj = interp_eval_expr(it, e->as.call.callee->as.member.obj);
            const char *mname = e->as.call.callee->as.member.field->data;

            xf_Value margs[65];
            margs[0] = obj;
            for (size_t i = 0; i < argc; i++) margs[i + 1] = args[i];
            size_t margc = argc + 1;

            xf_Value mr = interp_call_builtin(it, mname, margs, margc < 65 ? margc : 64);
            if (mr.state != XF_STATE_NAV) return mr;

            xf_Value callee = interp_eval_expr(it, e->as.call.callee);
            if (callee.type == XF_TYPE_FN && callee.state == XF_STATE_OK && callee.data.fn) {
                xf_Fn *fn = callee.data.fn;

                if (!fn->is_native) {
                    Scope *fn_sc = sym_push(it->syms, SCOPE_FN);
                    fn_sc->fn_ret_type = fn->return_type;

                    for (size_t i = 0; i < fn->param_count; i++) {
                        xf_Value av = i < argc ? args[i] : xf_val_undef(fn->params[i].type);
                        Symbol *ps = sym_declare(it->syms, fn->params[i].name,
                                                 SYM_PARAM, fn->params[i].type, e->loc);
                        if (ps) {
                            ps->value = av;
                            ps->state = av.state;
                            ps->is_defined = true;
                        }
                    }

                    it->returning = false;
                    interp_eval_stmt(it, (Stmt *)fn->body);
                    xf_Value ret = it->returning ? it->return_val : xf_val_null();
                    it->returning = false;

                    scope_free(sym_pop(it->syms));

                    if (fn->return_type != XF_TYPE_VOID && ret.state == XF_STATE_NULL)
                        ret = xf_val_nav(fn->return_type);

                    return ret;
                }

                if (fn->is_native && fn->native_v)
                    return fn->native_v(args, argc);
            }

            interp_error(it, e->loc, "unknown method '.%s'", mname);
            return xf_val_nav(XF_TYPE_VOID);
        }

        /* bare identifier: try builtin first */
        if (e->as.call.callee->kind == EXPR_IDENT) {
            const char *name = e->as.call.callee->as.ident.name->data;
            xf_Value r = interp_call_builtin(it, name, args, argc);
            if (r.state != XF_STATE_NAV) return r;
        }

        /* generic callable value */
        xf_Value callee = interp_eval_expr(it, e->as.call.callee);

        if (callee.type == XF_TYPE_FN && callee.state == XF_STATE_OK &&
            callee.data.fn && !callee.data.fn->is_native) {
            xf_Fn *fn = callee.data.fn;

            Scope *fn_sc = sym_push(it->syms, SCOPE_FN);
            fn_sc->fn_ret_type = fn->return_type;

            for (size_t i = 0; i < fn->param_count; i++) {
                xf_Value av = i < argc ? args[i] : xf_val_undef(fn->params[i].type);
                Symbol *ps = sym_declare(it->syms, fn->params[i].name,
                                         SYM_PARAM, fn->params[i].type, e->loc);
                if (ps) {
                    ps->value = av;
                    ps->state = av.state;
                    ps->is_defined = true;
                }
            }

            it->returning = false;
            interp_eval_stmt(it, (Stmt *)fn->body);
            xf_Value ret = it->returning ? it->return_val : xf_val_null();
            it->returning = false;

            scope_free(sym_pop(it->syms));

            if (fn->return_type != XF_TYPE_VOID && ret.state == XF_STATE_NULL)
                ret = xf_val_nav(fn->return_type);

            return ret;
        }

        if (callee.type == XF_TYPE_FN && callee.state == XF_STATE_OK &&
            callee.data.fn && callee.data.fn->is_native && callee.data.fn->native_v) {
            return callee.data.fn->native_v(args, argc);
        }

        interp_error(it, e->loc, "call to unresolved function");
        return xf_val_nav(XF_TYPE_VOID);
    }
        case EXPR_STATE: {
        xf_Value v = interp_eval_expr(it, e->as.introspect.operand);
        return xf_val_ok_num((double)v.state);
    }
    case EXPR_TYPE: {
        xf_Value v = interp_eval_expr(it, e->as.introspect.operand);
        return xf_val_ok_num((double)v.type);
    }
    case EXPR_LEN: {
        xf_Value v = interp_eval_expr(it, e->as.introspect.operand);
        if (v.state != XF_STATE_OK) return v;
        switch (v.type) {
            case XF_TYPE_STR:
                return xf_val_ok_num(v.data.str ? (double)v.data.str->len : 0.0);
            case XF_TYPE_ARR:
                return xf_val_ok_num(v.data.arr ? (double)v.data.arr->len : 0.0);
            case XF_TYPE_TUPLE:
    return xf_val_ok_num(v.data.tuple ? (double)xf_tuple_len(v.data.tuple) : 0.0);
            case XF_TYPE_MAP:
            case XF_TYPE_SET:
                return xf_val_ok_num(v.data.map ? (double)v.data.map->order_len : 0.0);
            case XF_TYPE_MODULE:
                if (v.data.mod) {
                    xf_Value r = xf_module_get(v.data.mod, "length");
                    if (r.state != XF_STATE_NAV) return r;
                }
                interp_error(it, e->loc, "module has no member 'length'");
                return xf_val_nav(XF_TYPE_NUM);
            default:
                interp_error(it, e->loc, ".len not defined for type '%s'",
                             XF_TYPE_NAMES[v.type]);
                return xf_val_nav(XF_TYPE_NUM);
        }
    }
    case EXPR_CAST: {
        xf_Value v = interp_eval_expr(it, e->as.cast.operand);
        if (v.state != XF_STATE_OK) return v;

        uint8_t target = e->as.cast.to_type;
        if (v.type == target) return v;

        switch (target) {
            case XF_TYPE_NUM:
                return xf_coerce_num(v);

            case XF_TYPE_STR:
                return xf_coerce_str(v);

            default:
                interp_error(it, e->loc, "cast to '%s' is not supported",
                             XF_TYPE_NAMES[target]);
                return xf_val_nav(target);
        }
    }
        case EXPR_MEMBER: {
        xf_Value obj = interp_eval_expr(it, e->as.member.obj);
        const char *field = e->as.member.field->data;
        if (obj.state == XF_STATE_OK &&
            obj.type  == XF_TYPE_MODULE && obj.data.mod) {
            xf_Value r = xf_module_get(obj.data.mod, field);
            if (r.state != XF_STATE_NAV) return r;
            interp_error(it, e->loc, "'%s' has no member '%s'",
                         obj.data.mod->name, field);
            return xf_val_nav(XF_TYPE_VOID);
        }
        if (strcmp(field, "state") == 0)
            return xf_val_ok_num((double)obj.state);
        if (strcmp(field, "type") == 0)
            return xf_val_ok_num((double)obj.type);
        if (strcmp(field, "len") == 0 || strcmp(field, "length") == 0) {
            if (obj.state != XF_STATE_OK) return obj;
            if (obj.type == XF_TYPE_STR)
                return xf_val_ok_num(obj.data.str ? (double)obj.data.str->len : 0.0);
            if (obj.type == XF_TYPE_ARR)
                return xf_val_ok_num(obj.data.arr ? (double)obj.data.arr->len : 0.0);
            if (obj.type == XF_TYPE_MAP || obj.type == XF_TYPE_SET)
                return xf_val_ok_num(obj.data.map ? (double)obj.data.map->used : 0.0);
            return xf_val_nav(XF_TYPE_NUM);
        }
        interp_error(it, e->loc, "unknown member '.%s'", field);
        return xf_val_nav(XF_TYPE_VOID);
    }
    case EXPR_PIPE_FN: {
        /* expr |> fn  — left becomes first argument of right.
         * Supports: bare ident, module.fn member, anonymous fn, fn value. */
        xf_Value left = interp_eval_expr(it, e->as.pipe_fn.left);
        Expr *rhs = e->as.pipe_fn.right;

        /* bare identifier — try builtins then symbol table */
        if (rhs->kind == EXPR_IDENT) {
            const char *name = rhs->as.ident.name->data;
            xf_Value r = interp_call_builtin(it, name, &left, 1);
            if (r.state != XF_STATE_NAV) return r;
            /* fall through to generic callable path */
        }

        /* module.fn — e.g. core.str.trim */
        if (rhs->kind == EXPR_MEMBER) {
            xf_Value obj    = interp_eval_expr(it, rhs->as.member.obj);
            const char *mname = rhs->as.member.field->data;
            xf_Value margs[2] = { obj, left };
            /* try as method call with obj as self */
            xf_Value mr = interp_call_builtin(it, mname, margs, 2);
            if (mr.state != XF_STATE_NAV) { xf_value_release(obj); return mr; }
            /* try resolving the member as a callable fn value */
            xf_Value callee = interp_eval_expr(it, rhs);
            xf_value_release(obj);
            if (callee.state == XF_STATE_OK && callee.type == XF_TYPE_FN && callee.data.fn) {
                xf_Fn *fn = callee.data.fn;
                if (fn->is_native && fn->native_v)
                    return fn->native_v(&left, 1);
                if (!fn->is_native) {
                    Scope *fn_sc = sym_push(it->syms, SCOPE_FN);
                    fn_sc->fn_ret_type = fn->return_type;
                    if (fn->param_count > 0) {
                        Symbol *ps = sym_declare(it->syms, fn->params[0].name,
                                                 SYM_PARAM, fn->params[0].type, e->loc);
                        if (ps) { ps->value = left; ps->state = left.state; ps->is_defined = true; }
                    }
                    it->returning = false;
                    interp_eval_stmt(it, (Stmt *)fn->body);
                    xf_Value ret = it->returning ? it->return_val : xf_val_null();
                    it->returning = false;
                    scope_free(sym_pop(it->syms));
                    if (fn->return_type != XF_TYPE_VOID && ret.state == XF_STATE_NULL)
                        ret = xf_val_nav(fn->return_type);
                    return ret;
                }
            }
            interp_error(it, e->loc, "pipe target '%s' is not callable", mname);
            return xf_val_nav(XF_TYPE_VOID);
        }

        /* generic: evaluate right side as a value and call it */
        xf_Value callee = interp_eval_expr(it, rhs);
        if (callee.state == XF_STATE_OK && callee.type == XF_TYPE_FN && callee.data.fn) {
            xf_Fn *fn = callee.data.fn;
            if (fn->is_native && fn->native_v)
                return fn->native_v(&left, 1);
            if (!fn->is_native) {
                Scope *fn_sc = sym_push(it->syms, SCOPE_FN);
                fn_sc->fn_ret_type = fn->return_type;
                if (fn->param_count > 0) {
                    Symbol *ps = sym_declare(it->syms, fn->params[0].name,
                                             SYM_PARAM, fn->params[0].type, e->loc);
                    if (ps) { ps->value = left; ps->state = left.state; ps->is_defined = true; }
                }
                it->returning = false;
                interp_eval_stmt(it, (Stmt *)fn->body);
                xf_Value ret = it->returning ? it->return_val : xf_val_null();
                it->returning = false;
                scope_free(sym_pop(it->syms));
                if (fn->return_type != XF_TYPE_VOID && ret.state == XF_STATE_NULL)
                    ret = xf_val_nav(fn->return_type);
                return ret;
            }
        }
        interp_error(it, e->loc, "pipe target is not callable");
        return xf_val_nav(XF_TYPE_VOID);
    }
    case EXPR_FN: {
        /* Build a new fn value each evaluation. The body pointer is borrowed
         * from the AST — do NOT null it out (that would break lambdas in loops
         * by destroying the body on the first iteration). The AST owns the body
         * for the program lifetime; xf_fn_release does NOT free the body node. */
        xf_Fn *fn = build_fn(NULL, e->as.fn.return_type,
                              e->as.fn.params, e->as.fn.param_count,
                              e->as.fn.body);
        return xf_val_ok_fn(fn);
    }
    default:
        interp_error(it, e->loc, "unhandled expression kind %d", e->kind);
        return xf_val_nav(XF_TYPE_VOID);
    }
}
/* ── iter_collection ────────────────────────────────────────────────
 * Shared iteration kernel used by both STMT_FOR and STMT_FOR_SHORT.
 * Walks arr / tuple / map / set / str collections, binding key+val
 * loop variables and executing `body` for each element.
 * ------------------------------------------------------------------ */
static void iter_collection(Interp *it, xf_Value col,
                             LoopBind *iter_key, LoopBind *iter_val,
                             Stmt *body, Loc loc) {
    if (col.type == XF_TYPE_ARR && col.data.arr) {
        xf_arr_t *a = col.data.arr;
        for (size_t i = 0; i < a->len; i++) {
            sym_push(it->syms, SCOPE_LOOP);
            xf_Value keyv = xf_val_ok_num((double)i);
            bind_loop_index_value(it, iter_key, iter_val, keyv, a->items[i], loc);
            interp_eval_stmt(it, body);
            scope_free(sym_pop(it->syms));
            if (it->nexting)  { it->nexting  = false; continue; }
            if (it->breaking) { it->breaking = false; break; }
            if (it->returning || it->exiting || it->had_error) break;
        }
    } else if (col.type == XF_TYPE_TUPLE && col.data.tuple) {
        size_t n = xf_tuple_len(col.data.tuple);
        for (size_t i = 0; i < n; i++) {
            xf_Value tv = xf_tuple_get(col.data.tuple, i);
            sym_push(it->syms, SCOPE_LOOP);
            xf_Value keyv = xf_val_ok_num((double)i);
            bind_loop_index_value(it, iter_key, iter_val, keyv, tv, loc);
            interp_eval_stmt(it, body);
            scope_free(sym_pop(it->syms));
            if (it->nexting)  { it->nexting  = false; continue; }
            if (it->breaking) { it->breaking = false; break; }
            if (it->returning || it->exiting || it->had_error) break;
        }
    } else if ((col.type == XF_TYPE_MAP || col.type == XF_TYPE_SET) && col.data.map) {
        xf_map_t *m   = col.data.map;
        bool      is_set = (col.type == XF_TYPE_SET);
        for (size_t i = 0; i < m->order_len; i++) {
            xf_Str  *key = m->order[i];
            xf_Value kv  = xf_val_ok_str(key);
            xf_Value vv  = is_set ? xf_val_ok_str(key) : xf_map_get(m, key);
            sym_push(it->syms, SCOPE_LOOP);
            bind_loop_index_value(it, iter_key, iter_val, kv, vv, loc);
            interp_eval_stmt(it, body);
            scope_free(sym_pop(it->syms));
            xf_value_release(kv);
            if (is_set) xf_value_release(vv);
            if (it->nexting)  { it->nexting  = false; continue; }
            if (it->breaking) { it->breaking = false; break; }
            if (it->returning || it->exiting || it->had_error) break;
        }
    } else {
        interp_error(it, loc, "cannot iterate over type '%s'",
                     XF_TYPE_NAMES[col.type]);
    }
}

xf_Value interp_eval_stmt(Interp *it, Stmt *s) {
    if (!s) return xf_val_null();
    if (it->had_error || it->returning || it->exiting || it->nexting || it->breaking)
        return xf_val_null();

    switch (s->kind) {
    case STMT_BLOCK: {
        xf_Value last = xf_val_null();
        sym_push(it->syms, SCOPE_BLOCK);
        for (size_t i = 0; i < s->as.block.count; i++) {
            last = interp_eval_stmt(it, s->as.block.stmts[i]);
            if (it->returning || it->exiting || it->nexting ||
                it->breaking || it->had_error) break;
        }
        scope_free(sym_pop(it->syms));
        return last;
    }

    case STMT_EXPR:
        return interp_eval_expr(it, s->as.expr.expr);

    case STMT_VAR_DECL: {
        xf_Value init = s->as.var_decl.init
                         ? interp_eval_expr(it, s->as.var_decl.init)
                         : xf_val_undef(s->as.var_decl.type);

        if (init.state == XF_STATE_OK &&
            s->as.var_decl.type != XF_TYPE_VOID &&
            init.type != s->as.var_decl.type) {
            if (s->as.var_decl.type == XF_TYPE_NUM) {
                init = xf_coerce_num(init);
            } else if (s->as.var_decl.type == XF_TYPE_STR) {
                xf_Value old = init;
                init = xf_coerce_str(old);
                xf_value_release(old);
            }
        }

        Symbol *sym = sym_lookup_local(it->syms,
                          s->as.var_decl.name->data, s->as.var_decl.name->len);
        if (!sym)
            sym = sym_declare(it->syms, s->as.var_decl.name,
                              SYM_VAR, s->as.var_decl.type, s->loc);

        if (sym) {
            xf_value_release(sym->value);
            sym->value = init;
            sym->state = init.state;
            sym->is_defined = true;
        }
        return init;
    }

    case STMT_FN_DECL: {
        xf_Fn *fn = build_fn(s->as.fn_decl.name, s->as.fn_decl.return_type,
                             s->as.fn_decl.params, s->as.fn_decl.param_count,
                             s->as.fn_decl.body);
        xf_Value fv = xf_val_ok_fn(fn);

        Symbol *sym = sym_lookup_local(it->syms,
                          s->as.fn_decl.name->data, s->as.fn_decl.name->len);
        if (!sym)
            sym = sym_declare(it->syms, s->as.fn_decl.name,
                              SYM_FN, XF_TYPE_FN, s->loc);

        if (sym) {
            xf_value_release(sym->value);
            sym->value = fv;
            sym->state = XF_STATE_OK;
            sym->is_defined = true;
        }
        return xf_val_null();
    }

    case STMT_IF: {
        for (size_t i = 0; i < s->as.if_stmt.count; i++) {
            xf_Value cond = interp_eval_expr(it, s->as.if_stmt.branches[i].cond);
            if (is_truthy(cond)) {
                sym_push(it->syms, SCOPE_BLOCK);
                xf_Value r = interp_eval_stmt(it, s->as.if_stmt.branches[i].body);
                scope_free(sym_pop(it->syms));
                return r;
            }
        }
        if (s->as.if_stmt.els) {
            sym_push(it->syms, SCOPE_BLOCK);
            xf_Value r = interp_eval_stmt(it, s->as.if_stmt.els);
            scope_free(sym_pop(it->syms));
            return r;
        }
        return xf_val_null();
    }

    case STMT_WHILE: {
        sym_push(it->syms, SCOPE_LOOP);
        while (is_truthy(interp_eval_expr(it, s->as.while_stmt.cond))) {
            interp_eval_stmt(it, s->as.while_stmt.body);
            if (it->nexting)  { it->nexting = false; continue; }
            if (it->breaking) { it->breaking = false; break; }
            if (it->returning || it->exiting || it->had_error) break;
        }
        scope_free(sym_pop(it->syms));
        return xf_val_null();
    }

    case STMT_WHILE_SHORT: {
        sym_push(it->syms, SCOPE_LOOP);
        while (is_truthy(interp_eval_expr(it, s->as.while_short.cond))) {
            interp_eval_stmt(it, s->as.while_short.body);
            if (it->nexting)  { it->nexting = false; continue; }
            if (it->breaking) { it->breaking = false; break; }
            if (it->returning || it->exiting || it->had_error) break;
        }
        scope_free(sym_pop(it->syms));
        return xf_val_null();
    }
    case STMT_FOR: {
        xf_Value col = interp_eval_expr(it, s->as.for_stmt.collection);
        if (col.state != XF_STATE_OK) return col;
        iter_collection(it, col, s->as.for_stmt.iter_key,
                        s->as.for_stmt.iter_val, s->as.for_stmt.body, s->loc);
        xf_value_release(col);
        return xf_val_null();
    }
        case STMT_FOR_SHORT: {
        xf_Value col = interp_eval_expr(it, s->as.for_short.collection);
        if (col.state != XF_STATE_OK) return col;
        iter_collection(it, col, s->as.for_short.iter_key,
                        s->as.for_short.iter_val, s->as.for_short.body, s->loc);
        xf_value_release(col);
        return xf_val_null();
    }


    case STMT_RETURN:
        it->return_val = s->as.ret.value
                          ? interp_eval_expr(it, s->as.ret.value)
                          : xf_val_null();
        it->returning = true;
        return it->return_val;

    case STMT_NEXT:
        it->nexting = true;
        return xf_val_null();

    case STMT_EXIT:
        it->exiting = true;
        return xf_val_null();

    case STMT_BREAK:
        it->breaking = true;
        return xf_val_null();

    case STMT_PRINT: {
        xf_Value vals[64];
        size_t count = s->as.print.count < 64 ? s->as.print.count : 64;

        for (size_t i = 0; i < count; i++)
            vals[i] = interp_eval_expr(it, s->as.print.args[i]);

        FILE *_out = stdout;
        bool _close_out = false;

        if (s->as.print.redirect && s->as.print.redirect_op != 0) {
            xf_Value rv = interp_eval_expr(it, s->as.print.redirect);
            xf_Value rs = xf_coerce_str(rv);
            xf_value_release(rv);
            if (rs.state == XF_STATE_OK && rs.data.str) {
                FILE *cached = vm_redir_open(it->vm, rs.data.str->data,
                                             s->as.print.redirect_op);
                if (cached) _out = cached;
            }
            xf_value_release(rs);
        }

        uint8_t mode = (_out == stdout) ? IT_REC(it)->out_mode : XF_OUTFMT_TEXT;

        if (mode != XF_OUTFMT_TEXT) {
            print_structured(it, vals, count, mode);
        } else {
            for (size_t i = 0; i < count; i++) {
                if (i > 0) fputs(IT_REC(it)->ofs, _out);
                xf_print_value(_out, vals[i]);
            }
            fputs(IT_REC(it)->ors, _out);
        }

        if (_close_out) {
            if (s->as.print.redirect_op == 3) pclose(_out);
            else fclose(_out);
        }

        for (size_t i = 0; i < count; i++) xf_value_release(vals[i]);
        return xf_val_null();
    }

    case STMT_PRINTF: {
        if (s->as.printf_stmt.count == 0) return xf_val_null();

        xf_Value fmtv = interp_eval_expr(it, s->as.printf_stmt.args[0]);
        xf_Value fmts = xf_coerce_str(fmtv);
        xf_value_release(fmtv);

        if (fmts.state != XF_STATE_OK || !fmts.data.str) {
            xf_value_release(fmts);
            return xf_val_null();
        }

        xf_Value args[64];
        size_t argc = s->as.printf_stmt.count < 64 ? s->as.printf_stmt.count : 64;
        for (size_t i = 1; i < argc; i++)
            args[i] = interp_eval_expr(it, s->as.printf_stmt.args[i]);

        char buf[8192];
        xf_sprintf_impl(buf, sizeof(buf), fmts.data.str->data, args + 1, argc - 1);

        FILE *_pf_out = stdout;
        bool _pf_close = false;

        if (s->as.printf_stmt.redirect && s->as.printf_stmt.redirect_op != 0) {
            xf_Value rv = interp_eval_expr(it, s->as.printf_stmt.redirect);
            xf_Value rs = xf_coerce_str(rv);
            xf_value_release(rv);
            if (rs.state == XF_STATE_OK && rs.data.str) {
                FILE *cached = vm_redir_open(it->vm, rs.data.str->data,
                                             s->as.printf_stmt.redirect_op);
                if (cached) _pf_out = cached;
            }
            xf_value_release(rs);
        }

        fputs(buf, _pf_out);

        if (_pf_close) {
            if (s->as.printf_stmt.redirect_op == 3) pclose(_pf_out);
            else fclose(_pf_out);
        }

        for (size_t i = 1; i < argc; i++) xf_value_release(args[i]);
        xf_value_release(fmts);
        return xf_val_null();
    }

    case STMT_OUTFMT:
        if (s->as.outfmt.mode == XF_OUTFMT_JSON &&
            IT_REC(it)->out_mode != XF_OUTFMT_JSON)
            IT_REC(it)->headers_set = false;
        IT_REC(it)->out_mode = s->as.outfmt.mode;
        return xf_val_null();

    case STMT_IMPORT: {
        if (!s->as.import.path)
            return xf_val_null();

        const char *imp_path = s->as.import.path->data;
        FILE *imp_fp = fopen(imp_path, "r");
        if (!imp_fp) {
            interp_error(it, s->loc, "import: cannot open '%s'", imp_path);
            return xf_val_null();
        }

        fseek(imp_fp, 0, SEEK_END);
        long imp_sz = ftell(imp_fp);
        rewind(imp_fp);

        char *imp_src = malloc((size_t)imp_sz + 1);
        fread(imp_src, 1, (size_t)imp_sz, imp_fp);
        fclose(imp_fp);
        imp_src[imp_sz] = '\0';

        Lexer imp_lex;
        xf_lex_init(&imp_lex, imp_src, (size_t)imp_sz, XF_SRC_FILE, imp_path);
        xf_tokenize(&imp_lex);

        Program *imp_prog = parse(&imp_lex, it->syms);

        if (imp_prog) {
            for (size_t ii = 0; ii < imp_prog->count; ii++)
                interp_eval_top(it, imp_prog->items[ii]);
            ast_program_free(imp_prog);
        } else {
            interp_error(it, s->loc, "import: parse error in '%s'", imp_path);
        }

        xf_lex_free(&imp_lex);
        free(imp_src);
        return xf_val_null();
    }

    case STMT_DELETE: {
        Expr *tgt = s->as.delete.target;
        if (tgt && tgt->kind == EXPR_SUBSCRIPT) {
            xf_Value obj = interp_eval_expr(it, tgt->as.subscript.obj);
            xf_Value key = interp_eval_expr(it, tgt->as.subscript.key);

            if (obj.state == XF_STATE_OK && obj.type == XF_TYPE_ARR && obj.data.arr) {
                xf_Value ni = xf_coerce_num(key);
                if (ni.state == XF_STATE_OK)
                    xf_arr_delete(obj.data.arr, (size_t)ni.data.num);
            } else if (obj.state == XF_STATE_OK &&
                       (obj.type == XF_TYPE_MAP || obj.type == XF_TYPE_SET) &&
                       obj.data.map) {
                xf_Value ks = xf_coerce_str(key);
                if (ks.state == XF_STATE_OK && ks.data.str)
                    xf_map_delete(obj.data.map, ks.data.str);
                xf_value_release(ks);
            }

            xf_value_release(obj);
            xf_value_release(key);
        }
        return xf_val_null();
    }

    case STMT_SPAWN: {
        Expr *call_expr = s->as.spawn.call;
        if (!call_expr || call_expr->kind != EXPR_CALL) {
            xf_Value sv = interp_eval_expr(it, s->as.spawn.call);
            xf_value_release(sv);
            return xf_val_ok_num(0.0);
        }

        xf_Value callee = interp_eval_expr(it, call_expr->as.call.callee);
        size_t argc = call_expr->as.call.argc < 64 ? call_expr->as.call.argc : 64;
        xf_Value sargs[64];

        for (size_t i = 0; i < argc; i++)
            sargs[i] = interp_eval_expr(it, call_expr->as.call.args[i]);

        pthread_mutex_lock(&g_spawn_mu);
        if (g_spawn_count >= XF_SPAWN_MAX || callee.type != XF_TYPE_FN) {
            pthread_mutex_unlock(&g_spawn_mu);
            xf_value_release(callee);
            for (size_t i = 0; i < argc; i++) xf_value_release(sargs[i]);
            return xf_val_ok_num(0.0);
        }

        SpawnCtx *ctx = &g_spawn[g_spawn_count++];
        memset(ctx, 0, sizeof(*ctx));
        ctx->id = g_spawn_next++;
        ctx->vm = it->vm;
        ctx->parent_syms = it->syms;
        ctx->fn_val = callee;
        ctx->argc = argc;
        for (size_t i = 0; i < argc; i++) ctx->args[i] = sargs[i];
        uint32_t handle_id = ctx->id;
        pthread_mutex_unlock(&g_spawn_mu);

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        pthread_create(&ctx->tid, &attr, spawn_thread_fn, ctx);
        pthread_attr_destroy(&attr);

        return xf_val_ok_num((double)handle_id);
    }

    case STMT_JOIN: {
        xf_Value handle = interp_eval_expr(it, s->as.join.handle);
        xf_Value join_result = xf_val_null();

        if (handle.state == XF_STATE_OK && handle.type == XF_TYPE_NUM) {
            uint32_t hid = (uint32_t)handle.data.num;

            pthread_mutex_lock(&g_spawn_mu);
            SpawnCtx *found = NULL;
            size_t found_idx = 0;
            for (size_t i = 0; i < g_spawn_count; i++) {
                if (g_spawn[i].id == hid) {
                    found = &g_spawn[i];
                    found_idx = i;
                    break;
                }
            }
            pthread_t tid = found ? found->tid : 0;
            pthread_mutex_unlock(&g_spawn_mu);

            if (tid) {
                pthread_join(tid, NULL);
                pthread_mutex_lock(&g_spawn_mu);
                if (found) {
                    join_result = found->result;
                    g_spawn[found_idx] = g_spawn[--g_spawn_count];
                }
                pthread_mutex_unlock(&g_spawn_mu);
            }
        }

        xf_value_release(handle);
        return join_result;
    }

    case STMT_SUBST:
    case STMT_TRANS:
        return xf_val_null();

    default:
        interp_error(it, s->loc, "unhandled statement kind %d", s->kind);
        return xf_val_null();
    }
}
xf_Value interp_eval_top(Interp *it, TopLevel *top) {
    if (!top) return xf_val_null();
    switch (top->kind) {
        case TOP_BEGIN:
        case TOP_END:
            return interp_eval_stmt(it, top->as.begin_end.body);
        case TOP_RULE:
            if (top->as.rule.pattern) {
                xf_Value pat = interp_eval_expr(it, top->as.rule.pattern);
                if (!is_truthy(pat)) return xf_val_null();
            }
            return interp_eval_stmt(it, top->as.rule.body);
        case TOP_FN: {
            xf_Fn *fn = build_fn(top->as.fn.name, top->as.fn.return_type,
                                  top->as.fn.params, top->as.fn.param_count,
                                  top->as.fn.body);
            /* body stays in AST — not stolen */
            xf_Value fv = xf_val_ok_fn(fn);
            Symbol *sym = sym_lookup_local(it->syms,
                              top->as.fn.name->data, top->as.fn.name->len);
            if (!sym)
                sym = sym_declare(it->syms, top->as.fn.name, SYM_FN, XF_TYPE_FN, top->loc);
            if (sym) { xf_value_release(sym->value); sym->value = fv; sym->state = XF_STATE_OK; sym->is_defined = true; }
            return xf_val_null();
        }
        case TOP_STMT:
            return interp_eval_stmt(it, top->as.stmt.stmt);
        default:
            return xf_val_null();
    }
}
int interp_run_program(Interp *it, Program *prog) {
    for (size_t i = 0; i < prog->count; i++) {
        TopLevel *t = prog->items[i];
        if (t->kind == TOP_BEGIN || t->kind == TOP_END ||
            t->kind == TOP_RULE) continue;
        interp_eval_top(it, t);
        if (it->exiting || it->had_error) return it->had_error ? 1 : 0;
    }
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->items[i]->kind != TOP_BEGIN) continue;
        interp_eval_top(it, prog->items[i]);
        if (it->had_error) return 1;
        if (it->exiting) break;
    }
    return it->had_error ? 1 : 0;
}
void interp_run_end(Interp *it, Program *prog) {
    if (it->had_error) return;
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->items[i]->kind != TOP_END) continue;
        it->exiting = false;
        interp_eval_top(it, prog->items[i]);
        if (it->had_error) return;
    }
    /* flush and close all cached redirect handles now that all output is done */
    vm_redir_flush(it->vm);
}
/* Print all fields of the current record through the active output format.
 * Used for format-conversion passthrough (e.g. -f csv:json data.csv). */
void interp_print_record(Interp *it) {
    RecordCtx *_rc = IT_REC(it);
    size_t count = _rc->field_count;
    if (count == 0) return;

    xf_Value *vals = malloc(sizeof(xf_Value) * count);
    if (!vals) return;
    for (size_t i = 0; i < count; i++) {
        xf_Str *s = xf_str_from_cstr(_rc->fields[i]);
        vals[i] = xf_val_ok_str(s);
        xf_str_release(s);
    }
    print_structured(it, vals, count, _rc->out_mode);
    for (size_t i = 0; i < count; i++) xf_value_release(vals[i]);
    free(vals);
}

void interp_feed_record(Interp *it, Program *prog,
                        const char *rec, size_t len) {
    vm_split_record(it->vm, rec, len);

    /* JSON mode: capture first record as column headers, skip rule eval */
    if (IT_REC(it)->out_mode == XF_OUTFMT_JSON &&
        !IT_REC(it)->headers_set) {
        vm_capture_headers(it->vm);
        return;  /* header row is consumed, not passed through rules */
    }

    for (size_t i = 0; i < prog->count; i++) {
        TopLevel *t = prog->items[i];
        if (t->kind != TOP_RULE) continue;
        if (t->as.rule.pattern) {
            xf_Value pat = interp_eval_expr(it, t->as.rule.pattern);
            xf_value_release(pat);
            if (!is_truthy(pat)) continue;
        }
        it->nexting = false;
        interp_eval_stmt(it, t->as.rule.body);
        if (it->nexting) { it->nexting = false; break; }
        if (it->exiting || it->had_error) return;
    }
}
bool interp_compile_program(Interp *it, Program *prog) {
    (void)it; (void)prog;
    return true;
}
Chunk *interp_compile_expr(Interp *it, Expr *e, const char *name) {
    (void)it; (void)e;
    Chunk *c = malloc(sizeof(Chunk));
    chunk_init(c, name);
    chunk_write(c, OP_HALT, 0);
    return c;
}
Chunk *interp_compile_stmt(Interp *it, Stmt *s, const char *name) {
    (void)it; (void)s;
    Chunk *c = malloc(sizeof(Chunk));
    chunk_init(c, name );
    chunk_write(c, OP_HALT, 0);
    return c;
}