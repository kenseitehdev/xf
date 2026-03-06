#include "../include/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Internal alloc helpers
 * ============================================================ */

static Expr *expr_alloc(ExprKind kind, Loc loc) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = kind;
    e->loc  = loc;
    return e;
}

static Stmt *stmt_alloc(StmtKind kind, Loc loc) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->kind = kind;
    s->loc  = loc;
    return s;
}

static TopLevel *top_alloc(TopKind kind, Loc loc) {
    TopLevel *t = calloc(1, sizeof(TopLevel));
    t->kind = kind;
    t->loc  = loc;
    return t;
}


/* ============================================================
 * Expr constructors
 * ============================================================ */

Expr *ast_num(double val, Loc loc) {
    Expr *e = expr_alloc(EXPR_NUM, loc);
    e->as.num = val;
    return e;
}
Expr *ast_arr_lit(Expr **items, size_t count, Loc loc) {
    Expr *e = expr_alloc(EXPR_ARR_LIT, loc);   /* use your existing allocator */
    e->as.arr_lit.items = items;
    e->as.arr_lit.count = count;
    return e;
}
Expr *ast_map_lit(Expr **keys, Expr **vals, size_t count, Loc loc) {
    Expr *e = expr_alloc(EXPR_MAP_LIT, loc);
    e->as.map_lit.keys=keys; e->as.map_lit.vals=vals; e->as.map_lit.count=count;
    return e;
}
Expr *ast_set_lit(Expr **items, size_t count, Loc loc) {
    Expr *e = expr_alloc(EXPR_SET_LIT, loc);
    e->as.set_lit.items=items; e->as.set_lit.count=count;
    return e;
}
Expr *ast_str(xf_Str *val, Loc loc) {
    Expr *e = expr_alloc(EXPR_STR, loc);
    e->as.str.value = xf_str_retain(val);
    return e;
}

Expr *ast_regex(xf_Str *pattern, uint32_t flags, Loc loc) {
    Expr *e = expr_alloc(EXPR_REGEX, loc);
    e->as.regex.pattern = xf_str_retain(pattern);
    e->as.regex.flags   = flags;
    return e;
}

Expr *ast_ident(xf_Str *name, Loc loc) {
    Expr *e = expr_alloc(EXPR_IDENT, loc);
    e->as.ident.name = xf_str_retain(name);
    return e;
}

Expr *ast_field(int index, Loc loc) {
    Expr *e = expr_alloc(EXPR_FIELD, loc);
    e->as.field.index = index;
    return e;
}

Expr *ast_ivar(TokenKind var, Loc loc) {
    Expr *e = expr_alloc(EXPR_IVAR, loc);
    e->as.ivar.var = var;
    return e;
}

Expr *ast_svar(TokenKind var, Loc loc) {
    Expr *e = expr_alloc(EXPR_SVAR, loc);
    e->as.svar.var = var;
    return e;
}

Expr *ast_unary(UnOp op, Expr *operand, Loc loc) {
    Expr *e = expr_alloc(EXPR_UNARY, loc);
    e->as.unary.op      = op;
    e->as.unary.operand = operand;
    return e;
}

Expr *ast_binary(BinOp op, Expr *left, Expr *right, Loc loc) {
    Expr *e = expr_alloc(EXPR_BINARY, loc);
    e->as.binary.op    = op;
    e->as.binary.left  = left;
    e->as.binary.right = right;
    return e;
}

Expr *ast_ternary(Expr *cond, Expr *then, Expr *els, Loc loc) {
    Expr *e = expr_alloc(EXPR_TERNARY, loc);
    e->as.ternary.cond = cond;
    e->as.ternary.then = then;
    e->as.ternary.els  = els;
    return e;
}

Expr *ast_coalesce(Expr *left, Expr *right, Loc loc) {
    Expr *e = expr_alloc(EXPR_COALESCE, loc);
    e->as.coalesce.left  = left;
    e->as.coalesce.right = right;
    return e;
}

Expr *ast_assign(AssignOp op, Expr *target, Expr *value, Loc loc) {
    Expr *e = expr_alloc(EXPR_ASSIGN, loc);
    e->as.assign.op     = op;
    e->as.assign.target = target;
    e->as.assign.value  = value;
    return e;
}

Expr *ast_walrus(xf_Str *name, uint8_t type, Expr *value, Loc loc) {
    Expr *e = expr_alloc(EXPR_WALRUS, loc);
    e->as.walrus.name  = xf_str_retain(name);
    e->as.walrus.type  = type;
    e->as.walrus.value = value;
    return e;
}

Expr *ast_call(Expr *callee, Expr **args, size_t argc, Loc loc) {
    Expr *e = expr_alloc(EXPR_CALL, loc);
    e->as.call.callee = callee;
    e->as.call.args   = args;
    e->as.call.argc   = argc;
    return e;
}

Expr *ast_subscript(Expr *obj, Expr *key, Loc loc) {
    Expr *e = expr_alloc(EXPR_SUBSCRIPT, loc);
    e->as.subscript.obj = obj;
    e->as.subscript.key = key;
    return e;
}

Expr *ast_member(Expr *obj, xf_Str *field, Loc loc) {
    Expr *e = expr_alloc(EXPR_MEMBER, loc);
    e->as.member.obj   = obj;
    e->as.member.field = xf_str_retain(field);
    return e;
}

Expr *ast_state(Expr *operand, Loc loc) {
    Expr *e = expr_alloc(EXPR_STATE, loc);
    e->as.introspect.operand = operand;
    return e;
}

Expr *ast_type(Expr *operand, Loc loc) {
    Expr *e = expr_alloc(EXPR_TYPE, loc);
    e->as.introspect.operand = operand;
    return e;
}

Expr *ast_len(Expr *operand, Loc loc) {
    Expr *e = expr_alloc(EXPR_LEN, loc);
    e->as.introspect.operand = operand;
    return e;
}

Expr *ast_cast(uint8_t to_type, Expr *operand, Loc loc) {
    Expr *e = expr_alloc(EXPR_CAST, loc);
    e->as.cast.to_type = to_type;
    e->as.cast.operand = operand;
    return e;
}

Expr *ast_pipe_fn(Expr *left, Expr *right, Loc loc) {
    Expr *e = expr_alloc(EXPR_PIPE_FN, loc);
    e->as.pipe_fn.left  = left;
    e->as.pipe_fn.right = right;
    return e;
}

Expr *ast_spread(Expr *operand, Loc loc) {
    Expr *e = expr_alloc(EXPR_SPREAD, loc);
    e->as.spread.operand = operand;
    return e;
}

Expr *ast_fn(uint8_t ret, Param *params, size_t pc, Stmt *body, Loc loc) {
    Expr *e = expr_alloc(EXPR_FN, loc);
    e->as.fn.return_type  = ret;
    e->as.fn.params       = params;
    e->as.fn.param_count  = pc;
    e->as.fn.body         = body;
    return e;
}


/* ============================================================
 * Stmt constructors
 * ============================================================ */

Stmt *ast_block(Stmt **stmts, size_t count, Loc loc) {
    Stmt *s = stmt_alloc(STMT_BLOCK, loc);
    s->as.block.stmts = stmts;
    s->as.block.count = count;
    return s;
}

Stmt *ast_expr_stmt(Expr *expr, Loc loc) {
    Stmt *s = stmt_alloc(STMT_EXPR, loc);
    s->as.expr.expr = expr;
    return s;
}

Stmt *ast_var_decl(uint8_t type, xf_Str *name, Expr *init, Loc loc) {
    Stmt *s = stmt_alloc(STMT_VAR_DECL, loc);
    s->as.var_decl.type = type;
    s->as.var_decl.name = xf_str_retain(name);
    s->as.var_decl.init = init;
    return s;
}

Stmt *ast_fn_decl(uint8_t ret, xf_Str *name,
                  Param *params, size_t pc, Stmt *body, Loc loc) {
    Stmt *s = stmt_alloc(STMT_FN_DECL, loc);
    s->as.fn_decl.return_type  = ret;
    s->as.fn_decl.name         = xf_str_retain(name);
    s->as.fn_decl.params       = params;
    s->as.fn_decl.param_count  = pc;
    s->as.fn_decl.body         = body;
    return s;
}

Stmt *ast_if(Branch *branches, size_t count, Stmt *els, Loc loc) {
    Stmt *s = stmt_alloc(STMT_IF, loc);
    s->as.if_stmt.branches = branches;
    s->as.if_stmt.count    = count;
    s->as.if_stmt.els      = els;
    return s;
}

Stmt *ast_while(Expr *cond, Stmt *body, Loc loc) {
    Stmt *s = stmt_alloc(STMT_WHILE, loc);
    s->as.while_stmt.cond = cond;
    s->as.while_stmt.body = body;
    return s;
}

Stmt *ast_for(xf_Str *iter, Expr *collection, Stmt *body, Loc loc) {
    Stmt *s = stmt_alloc(STMT_FOR, loc);
    s->as.for_stmt.iter       = xf_str_retain(iter);
    s->as.for_stmt.collection = collection;
    s->as.for_stmt.body       = body;
    return s;
}

Stmt *ast_while_short(Expr *cond, Stmt *body, Loc loc) {
    Stmt *s = stmt_alloc(STMT_WHILE_SHORT, loc);
    s->as.while_short.cond = cond;
    s->as.while_short.body = body;
    return s;
}

Stmt *ast_for_short(Expr *collection, xf_Str *iter, Stmt *body, Loc loc) {
    Stmt *s = stmt_alloc(STMT_FOR_SHORT, loc);
    s->as.for_short.collection = collection;
    s->as.for_short.iter       = xf_str_retain(iter);
    s->as.for_short.body       = body;
    return s;
}

Stmt *ast_return(Expr *value, Loc loc) {
    Stmt *s = stmt_alloc(STMT_RETURN, loc);
    s->as.ret.value = value;
    return s;
}

Stmt *ast_next(Loc loc) { return stmt_alloc(STMT_NEXT, loc); }
Stmt *ast_exit(Loc loc) { return stmt_alloc(STMT_EXIT, loc); }

Stmt *ast_print(Expr **args, size_t count, Expr *redirect, Loc loc) {
    Stmt *s = stmt_alloc(STMT_PRINT, loc);
    s->as.print.args     = args;
    s->as.print.count    = count;
    s->as.print.redirect = redirect;
    return s;
}

Stmt *ast_printf_stmt(Expr **args, size_t count, Expr *redirect, Loc loc) {
    Stmt *s = stmt_alloc(STMT_PRINTF, loc);
    s->as.printf_stmt.args     = args;
    s->as.printf_stmt.count    = count;
    s->as.printf_stmt.redirect = redirect;
    return s;
}

Stmt *ast_outfmt(uint8_t mode, Loc loc) {
    Stmt *s = stmt_alloc(STMT_OUTFMT, loc);
    s->as.outfmt.mode = mode;
    return s;
}

Stmt *ast_import(xf_Str *path, Loc loc) {
    Stmt *s = stmt_alloc(STMT_IMPORT, loc);
    s->as.import.path = xf_str_retain(path);
    return s;
}

Stmt *ast_delete(Expr *target, Loc loc) {
    Stmt *s = stmt_alloc(STMT_DELETE, loc);
    s->as.delete.target = target;
    return s;
}

Stmt *ast_spawn(Expr *call, Loc loc) {
    Stmt *s = stmt_alloc(STMT_SPAWN, loc);
    s->as.spawn.call = call;
    return s;
}

Stmt *ast_join(Expr *handle, Loc loc) {
    Stmt *s = stmt_alloc(STMT_JOIN, loc);
    s->as.join.handle = handle;
    return s;
}

Stmt *ast_subst(xf_Str *pat, xf_Str *rep, uint32_t flags, Expr *tgt, Loc loc) {
    Stmt *s = stmt_alloc(STMT_SUBST, loc);
    s->as.subst.pattern     = xf_str_retain(pat);
    s->as.subst.replacement = xf_str_retain(rep);
    s->as.subst.flags       = flags;
    s->as.subst.target      = tgt;
    return s;
}

Stmt *ast_trans(xf_Str *from, xf_Str *to, Expr *target, Loc loc) {
    Stmt *s = stmt_alloc(STMT_TRANS, loc);
    s->as.trans.from   = xf_str_retain(from);
    s->as.trans.to     = xf_str_retain(to);
    s->as.trans.target = target;
    return s;
}


/* ============================================================
 * TopLevel constructors
 * ============================================================ */

TopLevel *ast_top_begin(Stmt *body, Loc loc) {
    TopLevel *t = top_alloc(TOP_BEGIN, loc);
    t->as.begin_end.body = body;
    return t;
}

TopLevel *ast_top_end(Stmt *body, Loc loc) {
    TopLevel *t = top_alloc(TOP_END, loc);
    t->as.begin_end.body = body;
    return t;
}

TopLevel *ast_top_rule(Expr *pattern, Stmt *body, Loc loc) {
    TopLevel *t = top_alloc(TOP_RULE, loc);
    t->as.rule.pattern = pattern;
    t->as.rule.body    = body;
    return t;
}

TopLevel *ast_top_fn(uint8_t ret, xf_Str *name,
                     Param *params, size_t pc, Stmt *body, Loc loc) {
    TopLevel *t = top_alloc(TOP_FN, loc);
    t->as.fn.return_type = ret;
    t->as.fn.name        = xf_str_retain(name);
    t->as.fn.params      = params;
    t->as.fn.param_count = pc;
    t->as.fn.body        = body;
    return t;
}

TopLevel *ast_top_stmt(Stmt *stmt, Loc loc) {
    TopLevel *t = top_alloc(TOP_STMT, loc);
    t->as.stmt.stmt = stmt;
    return t;
}


/* ============================================================
 * Program
 * ============================================================ */

Program *ast_program_new(const char *source) {
    Program *p  = calloc(1, sizeof(Program));
    p->source   = source;
    p->capacity = 16;
    p->items    = malloc(sizeof(TopLevel *) * p->capacity);
    return p;
}

void ast_program_push(Program *p, TopLevel *item) {
    if (p->count >= p->capacity) {
        p->capacity *= 2;
        p->items = realloc(p->items, sizeof(TopLevel *) * p->capacity);
    }
    p->items[p->count++] = item;
}

void ast_program_free(Program *p) {
    if (!p) return;
    for (size_t i = 0; i < p->count; i++)
        ast_top_free(p->items[i]);
    free(p->items);
    free(p);
}


/* ============================================================
 * Free
 * ============================================================ */

void ast_expr_free(Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_STR:       xf_str_release(e->as.str.value);       break;
        case EXPR_REGEX:     xf_str_release(e->as.regex.pattern);    break;
        case EXPR_IDENT:     xf_str_release(e->as.ident.name);       break;
        case EXPR_UNARY:     ast_expr_free(e->as.unary.operand);      break;
        case EXPR_BINARY:
            ast_expr_free(e->as.binary.left);
            ast_expr_free(e->as.binary.right);
            break;
        case EXPR_TERNARY:
            ast_expr_free(e->as.ternary.cond);
            ast_expr_free(e->as.ternary.then);
            ast_expr_free(e->as.ternary.els);
            break;
        case EXPR_COALESCE:
            ast_expr_free(e->as.coalesce.left);
            ast_expr_free(e->as.coalesce.right);
            break;
        case EXPR_ASSIGN:
            ast_expr_free(e->as.assign.target);
            ast_expr_free(e->as.assign.value);
            break;
        case EXPR_WALRUS:
            xf_str_release(e->as.walrus.name);
            ast_expr_free(e->as.walrus.value);
            break;
        case EXPR_CALL:
            ast_expr_free(e->as.call.callee);
            for (size_t i = 0; i < e->as.call.argc; i++)
                ast_expr_free(e->as.call.args[i]);
            free(e->as.call.args);
            break;
        case EXPR_SUBSCRIPT:
            ast_expr_free(e->as.subscript.obj);
            ast_expr_free(e->as.subscript.key);
            break;
        case EXPR_MEMBER:
            ast_expr_free(e->as.member.obj);
            xf_str_release(e->as.member.field);
            break;
        case EXPR_STATE:
        case EXPR_TYPE:
            ast_expr_free(e->as.introspect.operand);
            break;
        case EXPR_ARR_LIT: for(size_t i=0;i<e->as.arr_lit.count;i++) ast_expr_free(e->as.arr_lit.items[i]); free(e->as.arr_lit.items); break;
        case EXPR_MAP_LIT: for(size_t i=0;i<e->as.map_lit.count;i++){ast_expr_free(e->as.map_lit.keys[i]);ast_expr_free(e->as.map_lit.vals[i]);} free(e->as.map_lit.keys); free(e->as.map_lit.vals); break;
        case EXPR_SET_LIT: for(size_t i=0;i<e->as.set_lit.count;i++) ast_expr_free(e->as.set_lit.items[i]); free(e->as.set_lit.items); break;
        case EXPR_PIPE_FN:
            ast_expr_free(e->as.pipe_fn.left);
            ast_expr_free(e->as.pipe_fn.right);
            break;
        case EXPR_SPREAD:
            ast_expr_free(e->as.spread.operand);
            break;
        case EXPR_FN:
            for (size_t i = 0; i < e->as.fn.param_count; i++) {
                xf_str_release(e->as.fn.params[i].name);
                ast_expr_free(e->as.fn.params[i].default_val);
            }
            free(e->as.fn.params);
            ast_stmt_free(e->as.fn.body);
            break;
        default: break;
    }
    free(e);
}

void ast_stmt_free(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_BLOCK:
            for (size_t i = 0; i < s->as.block.count; i++)
                ast_stmt_free(s->as.block.stmts[i]);
            free(s->as.block.stmts);
            break;
        case STMT_EXPR:    ast_expr_free(s->as.expr.expr); break;
        case STMT_VAR_DECL:
            xf_str_release(s->as.var_decl.name);
            ast_expr_free(s->as.var_decl.init);
            break;
        case STMT_FN_DECL:
            xf_str_release(s->as.fn_decl.name);
            for (size_t i = 0; i < s->as.fn_decl.param_count; i++) {
                xf_str_release(s->as.fn_decl.params[i].name);
                ast_expr_free(s->as.fn_decl.params[i].default_val);
            }
            free(s->as.fn_decl.params);
            ast_stmt_free(s->as.fn_decl.body);
            break;
        case STMT_IF:
            for (size_t i = 0; i < s->as.if_stmt.count; i++) {
                ast_expr_free(s->as.if_stmt.branches[i].cond);
                ast_stmt_free(s->as.if_stmt.branches[i].body);
            }
            free(s->as.if_stmt.branches);
            ast_stmt_free(s->as.if_stmt.els);
            break;
        case STMT_WHILE:
            ast_expr_free(s->as.while_stmt.cond);
            ast_stmt_free(s->as.while_stmt.body);
            break;
        case STMT_FOR:
            xf_str_release(s->as.for_stmt.iter);
            ast_expr_free(s->as.for_stmt.collection);
            ast_stmt_free(s->as.for_stmt.body);
            break;
        case STMT_WHILE_SHORT:
            ast_expr_free(s->as.while_short.cond);
            ast_stmt_free(s->as.while_short.body);
            break;
        case STMT_FOR_SHORT:
            ast_expr_free(s->as.for_short.collection);
            xf_str_release(s->as.for_short.iter);
            ast_stmt_free(s->as.for_short.body);
            break;
        case STMT_RETURN: ast_expr_free(s->as.ret.value);      break;
        case STMT_PRINT:
            for (size_t i = 0; i < s->as.print.count; i++)
                ast_expr_free(s->as.print.args[i]);
            free(s->as.print.args);
            ast_expr_free(s->as.print.redirect);
            break;
        case STMT_PRINTF:
            for (size_t i = 0; i < s->as.printf_stmt.count; i++)
                ast_expr_free(s->as.printf_stmt.args[i]);
            free(s->as.printf_stmt.args);
            ast_expr_free(s->as.printf_stmt.redirect);
            break;
        case STMT_OUTFMT: break;
        case STMT_IMPORT: xf_str_release(s->as.import.path);   break;
        case STMT_DELETE: ast_expr_free(s->as.delete.target);   break;
        case STMT_SPAWN:  ast_expr_free(s->as.spawn.call);      break;
        case STMT_JOIN:   ast_expr_free(s->as.join.handle);     break;
        case STMT_SUBST:
            xf_str_release(s->as.subst.pattern);
            xf_str_release(s->as.subst.replacement);
            ast_expr_free(s->as.subst.target);
            break;
        case STMT_TRANS:
            xf_str_release(s->as.trans.from);
            xf_str_release(s->as.trans.to);
            ast_expr_free(s->as.trans.target);
            break;
        default: break;
    }
    free(s);
}

void ast_top_free(TopLevel *t) {
    if (!t) return;
    switch (t->kind) {
        case TOP_BEGIN:
        case TOP_END:  ast_stmt_free(t->as.begin_end.body); break;
        case TOP_RULE:
            ast_expr_free(t->as.rule.pattern);
            ast_stmt_free(t->as.rule.body);
            break;
        case TOP_FN:
            xf_str_release(t->as.fn.name);
            for (size_t i = 0; i < t->as.fn.param_count; i++) {
                xf_str_release(t->as.fn.params[i].name);
                ast_expr_free(t->as.fn.params[i].default_val);
            }
            free(t->as.fn.params);
            ast_stmt_free(t->as.fn.body);
            break;
        case TOP_STMT: ast_stmt_free(t->as.stmt.stmt); break;
    }
    free(t);
}


/* ============================================================
 * Debug printer
 * ============================================================ */

static void indent(int n) { for (int i = 0; i < n*2; i++) putchar(' '); }

static const char *binop_str(BinOp op) {
    switch (op) {
        case BINOP_ADD: return "+";   case BINOP_SUB:      return "-";
        case BINOP_MUL: return "*";   case BINOP_DIV:      return "/";
        case BINOP_MOD: return "%";   case BINOP_POW:      return "^";
        case BINOP_EQ:  return "==";  case BINOP_NEQ:      return "!=";
        case BINOP_LT:  return "<";   case BINOP_GT:       return ">";
        case BINOP_LTE: return "<=";  case BINOP_GTE:      return ">=";
        case BINOP_SPACESHIP: return "<=>";
        case BINOP_AND: return "&&";  case BINOP_OR:       return "||";
        case BINOP_MATCH:  return "~"; case BINOP_NMATCH:  return "!~";
        case BINOP_CONCAT: return ".."; case BINOP_PIPE:   return "|";
        default: return "?";
    }
}

static const char *type_name(uint8_t t) { return XF_TYPE_NAMES[t < XF_TYPE_COUNT ? t : 0]; }

void ast_expr_print(const Expr *e, int d) {
    if (!e) { indent(d); printf("(null)\n"); return; }
    indent(d);
    switch (e->kind) {
        case EXPR_NUM:   printf("NUM %g\n", e->as.num); break;
        case EXPR_STR:   printf("STR \"%s\"\n", e->as.str.value->data); break;
        case EXPR_REGEX: printf("REGEX /%s/\n",  e->as.regex.pattern->data); break;
        case EXPR_IDENT: printf("IDENT %s\n",    e->as.ident.name->data); break;
        case EXPR_FIELD: printf("FIELD $%d\n",   e->as.field.index); break;
        case EXPR_IVAR:  printf("IVAR %s\n",     xf_token_kind_name(e->as.ivar.var)); break;
        case EXPR_SVAR:  printf("SVAR %s\n",     xf_token_kind_name(e->as.svar.var)); break;
        case EXPR_ARR_LIT: printf("ARR_LIT(%zu)\n",e->as.arr_lit.count); break;
        case EXPR_MAP_LIT: printf("MAP_LIT(%zu)\n",e->as.map_lit.count); break;
        case EXPR_SET_LIT: printf("SET_LIT(%zu)\n",e->as.set_lit.count); break;
        case EXPR_UNARY:
            printf("UNARY %d\n", e->as.unary.op);
            ast_expr_print(e->as.unary.operand, d+1);
            break;
        case EXPR_BINARY:
            printf("BINARY %s\n", binop_str(e->as.binary.op));
            ast_expr_print(e->as.binary.left,  d+1);
            ast_expr_print(e->as.binary.right, d+1);
            break;
        case EXPR_TERNARY:
            printf("TERNARY\n");
            ast_expr_print(e->as.ternary.cond, d+1);
            ast_expr_print(e->as.ternary.then, d+1);
            ast_expr_print(e->as.ternary.els,  d+1);
            break;
        case EXPR_COALESCE:
            printf("COALESCE ??\n");
            ast_expr_print(e->as.coalesce.left,  d+1);
            ast_expr_print(e->as.coalesce.right, d+1);
            break;
        case EXPR_ASSIGN:
            printf("ASSIGN\n");
            ast_expr_print(e->as.assign.target, d+1);
            ast_expr_print(e->as.assign.value,  d+1);
            break;
        case EXPR_WALRUS:
            printf("WALRUS %s:%s\n",
                   e->as.walrus.name->data, type_name(e->as.walrus.type));
            ast_expr_print(e->as.walrus.value, d+1);
            break;
        case EXPR_CALL:
            printf("CALL (argc=%zu)\n", e->as.call.argc);
            ast_expr_print(e->as.call.callee, d+1);
            for (size_t i = 0; i < e->as.call.argc; i++)
                ast_expr_print(e->as.call.args[i], d+1);
            break;
        case EXPR_SUBSCRIPT:
            printf("SUBSCRIPT\n");
            ast_expr_print(e->as.subscript.obj, d+1);
            ast_expr_print(e->as.subscript.key, d+1);
            break;
        case EXPR_MEMBER:
            printf("MEMBER .%s\n", e->as.member.field->data);
            ast_expr_print(e->as.member.obj, d+1);
            break;
        case EXPR_STATE:
            printf("STATE\n");
            ast_expr_print(e->as.introspect.operand, d+1);
            break;
        case EXPR_TYPE:
            printf("TYPE\n");
            ast_expr_print(e->as.introspect.operand, d+1);
            break;
        case EXPR_PIPE_FN:
            printf("PIPE_FN |>\n");
            ast_expr_print(e->as.pipe_fn.left,  d+1);
            ast_expr_print(e->as.pipe_fn.right, d+1);
            break;
        case EXPR_SPREAD:
            printf("SPREAD\n");
            ast_expr_print(e->as.spread.operand, d+1);
            break;
        case EXPR_FN:
            printf("FN_LITERAL ->%s (params=%zu)\n",
                   type_name(e->as.fn.return_type), e->as.fn.param_count);
            ast_stmt_print(e->as.fn.body, d+1);
            break;
        default:
            printf("EXPR(%d)\n", e->kind);
    }
}

void ast_stmt_print(const Stmt *s, int d) {
    if (!s) return;
    indent(d);
    switch (s->kind) {
        case STMT_BLOCK:
            printf("BLOCK (%zu)\n", s->as.block.count);
            for (size_t i = 0; i < s->as.block.count; i++)
                ast_stmt_print(s->as.block.stmts[i], d+1);
            break;
        case STMT_EXPR:
            printf("EXPR_STMT\n");
            ast_expr_print(s->as.expr.expr, d+1);
            break;
        case STMT_VAR_DECL:
            printf("VAR %s:%s\n",
                   s->as.var_decl.name->data, type_name(s->as.var_decl.type));
            if (s->as.var_decl.init) ast_expr_print(s->as.var_decl.init, d+1);
            break;
        case STMT_FN_DECL:
            printf("FN %s ->%s (params=%zu)\n",
                   s->as.fn_decl.name->data,
                   type_name(s->as.fn_decl.return_type),
                   s->as.fn_decl.param_count);
            ast_stmt_print(s->as.fn_decl.body, d+1);
            break;
        case STMT_IF:
            printf("IF (%zu branches)\n", s->as.if_stmt.count);
            for (size_t i = 0; i < s->as.if_stmt.count; i++) {
                indent(d+1); printf("COND\n");
                ast_expr_print(s->as.if_stmt.branches[i].cond, d+2);
                ast_stmt_print(s->as.if_stmt.branches[i].body, d+2);
            }
            if (s->as.if_stmt.els) {
                indent(d+1); printf("ELSE\n");
                ast_stmt_print(s->as.if_stmt.els, d+2);
            }
            break;
        case STMT_WHILE:
            printf("WHILE\n");
            ast_expr_print(s->as.while_stmt.cond, d+1);
            ast_stmt_print(s->as.while_stmt.body, d+1);
            break;
        case STMT_FOR:
            printf("FOR %s in\n", s->as.for_stmt.iter->data);
            ast_expr_print(s->as.for_stmt.collection, d+1);
            ast_stmt_print(s->as.for_stmt.body, d+1);
            break;
        case STMT_WHILE_SHORT:
            printf("WHILE_SHORT <>\n");
            ast_expr_print(s->as.while_short.cond, d+1);
            ast_stmt_print(s->as.while_short.body, d+1);
            break;
        case STMT_FOR_SHORT:
            printf("FOR_SHORT [%s] >\n", s->as.for_short.iter->data);
            ast_expr_print(s->as.for_short.collection, d+1);
            ast_stmt_print(s->as.for_short.body, d+1);
            break;
        case STMT_RETURN:
            printf("RETURN\n");
            if (s->as.ret.value) ast_expr_print(s->as.ret.value, d+1);
            break;
        case STMT_NEXT:   printf("NEXT\n");   break;
        case STMT_EXIT:   printf("EXIT\n");   break;
        case STMT_PRINT:
            printf("PRINT (%zu args)\n", s->as.print.count);
            for (size_t i = 0; i < s->as.print.count; i++)
                ast_expr_print(s->as.print.args[i], d+1);
            break;
        case STMT_PRINTF:
            printf("PRINTF (%zu args)\n", s->as.printf_stmt.count);
            for (size_t i = 0; i < s->as.printf_stmt.count; i++)
                ast_expr_print(s->as.printf_stmt.args[i], d+1);
            break;
        case STMT_OUTFMT:
            printf("OUTFMT %u\n", s->as.outfmt.mode);
            break;
        case STMT_IMPORT: printf("IMPORT \"%s\"\n", s->as.import.path->data); break;
        case STMT_DELETE:
            printf("DELETE\n");
            ast_expr_print(s->as.delete.target, d+1);
            break;
        case STMT_SPAWN:
            printf("SPAWN\n");
            ast_expr_print(s->as.spawn.call, d+1);
            break;
        case STMT_JOIN:
            printf("JOIN\n");
            ast_expr_print(s->as.join.handle, d+1);
            break;
        case STMT_SUBST:
            printf("SUBST s/%s/%s/\n",
                   s->as.subst.pattern->data, s->as.subst.replacement->data);
            break;
        case STMT_TRANS:
            printf("TRANS y/%s/%s/\n",
                   s->as.trans.from->data, s->as.trans.to->data);
            break;
        default:
            printf("STMT(%d)\n", s->kind);
    }
}

void ast_program_print(const Program *p) {
    if (!p) return;
    printf("PROGRAM \"%s\" (%zu items)\n", p->source, p->count);
    for (size_t i = 0; i < p->count; i++) {
        TopLevel *t = p->items[i];
        printf("[%zu] ", i);
        switch (t->kind) {
            case TOP_BEGIN:
                printf("BEGIN\n");
                ast_stmt_print(t->as.begin_end.body, 1);
                break;
            case TOP_END:
                printf("END\n");
                ast_stmt_print(t->as.begin_end.body, 1);
                break;
            case TOP_RULE:
                printf("RULE\n");
                if (t->as.rule.pattern) {
                    indent(1); printf("PATTERN\n");
                    ast_expr_print(t->as.rule.pattern, 2);
                } else {
                    indent(1); printf("PATTERN (always)\n");
                }
                ast_stmt_print(t->as.rule.body, 1);
                break;
            case TOP_FN:
                printf("FN %s ->%s (params=%zu)\n",
                       t->as.fn.name->data,
                       type_name(t->as.fn.return_type),
                       t->as.fn.param_count);
                ast_stmt_print(t->as.fn.body, 1);
                break;
            case TOP_STMT:
                printf("STMT\n");
                ast_stmt_print(t->as.stmt.stmt, 1);
                break;
        }
    }
}
