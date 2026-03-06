#include "../include/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Internal parser helpers
 * ============================================================ */

static Token *cur(Parser *p) {
    return &p->lex->tokens[p->pos];
}

static Token *peek_at(Parser *p, size_t offset) {
    size_t idx = p->pos + offset;
    if (idx >= p->lex->count) idx = p->lex->count - 1;
    return &p->lex->tokens[idx];
}

static TokenKind cur_kind(Parser *p) { return cur(p)->kind; }

static bool check(Parser *p, TokenKind k) { return cur_kind(p) == k; }

static Token *advance(Parser *p) {
    Token *t = cur(p);
    if (t->kind != TK_EOF) p->pos++;
    return t;
}

static bool match_tok(Parser *p, TokenKind k) {
    if (!check(p, k)) return false;
    advance(p);
    return true;
}

static Token *expect(Parser *p, TokenKind k, const char *msg) {
    if (check(p, k)) return advance(p);
    p->had_error  = true;
    p->err_loc    = cur(p)->loc;
    snprintf(p->err_msg, sizeof(p->err_msg),
             "%s — got '%s' at %s:%u:%u",
             msg, xf_token_kind_name(cur_kind(p)),
             cur(p)->loc.source, cur(p)->loc.line, cur(p)->loc.col);
    return cur(p);
}

static void parser_warning(Parser *p, const char *msg) {
    fprintf(stdout, "WARN: %s at %s:%u:%u\n",
            msg, cur(p)->loc.source, cur(p)->loc.line, cur(p)->loc.col);
}

static void parser_error(Parser *p, const char *msg) {
    if (p->panic_mode) return;
    p->had_error  = true;
    p->panic_mode = true;
    p->err_loc    = cur(p)->loc;
    snprintf(p->err_msg, sizeof(p->err_msg),
             "%s at %s:%u:%u",
             msg, cur(p)->loc.source, cur(p)->loc.line, cur(p)->loc.col);
    fprintf(stdout, "ERR ──────────────────────────────────────────────\r\n");
    fprintf(stdout, "  %s\r\n", p->err_msg);
    fprintf(stdout, "──────────────────────────────────────────────\r\n");
}

/* synchronize after error — skip to next safe boundary */
static void synchronize(Parser *p) {
    p->panic_mode = false;
    while (cur_kind(p) != TK_EOF) {
        if (cur_kind(p) == TK_SEMICOLON) { advance(p); return; }
        switch (cur_kind(p)) {
            case TK_KW_FN:
            case TK_KW_NUM: case TK_KW_STR: case TK_KW_MAP:
            case TK_KW_SET: case TK_KW_ARR: case TK_KW_VOID:
            case TK_KW_IF:    case TK_KW_WHILE: case TK_KW_FOR:
            case TK_KW_BEGIN: case TK_KW_END:   case TK_KW_RETURN:
            case TK_KW_PRINT: case TK_LBRACE:
                return;
            default: advance(p); break;
        }
    }
}

static xf_Str *tok_to_str(Token *t) {
    if (t->kind == TK_STR)
        return xf_str_new(t->val.str.data, t->val.str.len);
    return xf_str_new(t->lexeme, t->lexeme_len);
}

/* true if current token is a type keyword */
static bool is_type_kw(Parser *p) {
    switch (cur_kind(p)) {
        case TK_KW_NUM: case TK_KW_STR: case TK_KW_MAP:
        case TK_KW_SET: case TK_KW_ARR: case TK_KW_FN:
        case TK_KW_VOID: return true;
        default: return false;
    }
}


/* ============================================================
 * Type keyword → XF_TYPE_*
 * ============================================================ */

uint8_t parse_type(Parser *p) {
    switch (cur_kind(p)) {
        case TK_KW_NUM:  advance(p); return XF_TYPE_NUM;
        case TK_KW_STR:  advance(p); return XF_TYPE_STR;
        case TK_KW_MAP:  advance(p); return XF_TYPE_MAP;
        case TK_KW_SET:  advance(p); return XF_TYPE_SET;
        case TK_KW_ARR:  advance(p); return XF_TYPE_ARR;
        case TK_KW_FN:   advance(p); return XF_TYPE_FN;
        case TK_KW_VOID: advance(p); return XF_TYPE_VOID;
        default:
            parser_error(p, "expected type keyword");
            return XF_TYPE_VOID;
    }
}


/* ============================================================
 * Parameter list:  (type name, type name = default, ...)
 * ============================================================ */

Param *parse_paramlist(Parser *p, size_t *out_count) {
    size_t cap = 4, count = 0;
    Param *params = malloc(sizeof(Param) * cap);

    expect(p, TK_LPAREN, "expected '(' in parameter list");

    while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
        if (count >= cap) { cap *= 2; params = realloc(params, sizeof(Param)*cap); }

        Param pr = {0};
        pr.loc  = cur(p)->loc;
        pr.type = is_type_kw(p) ? parse_type(p) : XF_TYPE_VOID;

        Token *name = expect(p, TK_IDENT, "expected parameter name");
        pr.name = tok_to_str(name);

        if (match_tok(p, TK_EQ))
            pr.default_val = parse_expr(p);

        params[count++] = pr;
        if (!match_tok(p, TK_COMMA)) break;
    }

    expect(p, TK_RPAREN, "expected ')' after parameter list");
    *out_count = count;
    return params;
}


/* ============================================================
 * Argument list:  (expr, expr, ...)
 * ============================================================ */

Expr **parse_arglist(Parser *p, size_t *out_count) {
    size_t cap = 4, count = 0;
    Expr **args = malloc(sizeof(Expr *) * cap);

    expect(p, TK_LPAREN, "expected '(' in argument list");

    while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
        if (count >= cap) { cap *= 2; args = realloc(args, sizeof(Expr*)*cap); }
        args[count++] = parse_expr(p);
        if (!match_tok(p, TK_COMMA)) break;
    }

    expect(p, TK_RPAREN, "expected ')' after argument list");
    *out_count = count;
    return args;
}


/* ============================================================
 * Expression parsing — precedence climbing
 * ============================================================ */

Expr *parse_primary(Parser *p) {
    Token *t = cur(p);
    Loc    loc = t->loc;
/* array literal: [a, b, c] */
if (t->kind == TK_LBRACKET) {
    advance(p);  // consume '['

    Expr **items = NULL;
    size_t count = 0;
    size_t cap = 0;

    if (!check(p, TK_RBRACKET)) {
        for (;;) {
            Expr *item = parse_expr(p);

            if (count == cap) {
                size_t ncap = cap ? cap * 2 : 4;
                Expr **tmp = realloc(items, ncap * sizeof(*tmp));
                if (!tmp) {
                    parser_error(p, "out of memory");
                    free(items);
                    return ast_num(0, loc);
                }
                items = tmp;
                cap = ncap;
            }

            items[count++] = item;

            if (match_tok(p, TK_COMMA)) {
                if (check(p, TK_RBRACKET))
                    break; /* allow trailing comma */
                continue;
            }

            break;
        }
    }

    expect(p, TK_RBRACKET, "expected ']' after array literal");

    return ast_arr_lit(items, count, loc);
}
    /* numeric literal */
    if (t->kind == TK_NUM) {
        advance(p);
        return ast_num(t->val.num, loc);
    }

    /* string literal */
    if (t->kind == TK_STR) {
        advance(p);
        xf_Str *s = xf_str_new(t->val.str.data, t->val.str.len);
        Expr *e = ast_str(s, loc);
        xf_str_release(s);
        return e;
    }

    /* regex literal */
    if (t->kind == TK_REGEX) {
        advance(p);
        xf_Str *pat = xf_str_from_cstr(t->val.re.pattern);
        Expr *e = ast_regex(pat, t->val.re.flags, loc);
        xf_str_release(pat);
        return e;
    }

    /* field variable: $0 $1 ... */
    if (t->kind == TK_FIELD) {
        advance(p);
        return ast_field(t->val.field_idx, loc);
    }

    /* implicit vars: $file $match $captures $err */
    if (t->kind == TK_VAR_FILE || t->kind == TK_VAR_MATCH ||
        t->kind == TK_VAR_CAPS || t->kind == TK_VAR_ERR) {
        advance(p);
        return ast_svar(t->kind, loc);
    }

    /* built-in record vars: NR NF FNR FS RS OFS ORS */
    if (t->kind == TK_VAR_NR  || t->kind == TK_VAR_NF  ||
        t->kind == TK_VAR_FNR || t->kind == TK_VAR_FS  ||
        t->kind == TK_VAR_RS  || t->kind == TK_VAR_OFS ||
        t->kind == TK_VAR_ORS) {
        advance(p);
        return ast_ivar(t->kind, loc);
    }

    /* identifier or fn call */
    if (t->kind == TK_IDENT) {
        advance(p);
        xf_Str *name = tok_to_str(t);
        Expr *e = ast_ident(name, loc);
        xf_str_release(name);
        return e;
    }

    /* anonymous fn literal:  fn(params) { body } */
    if (t->kind == TK_KW_FN) {
        advance(p);
        size_t pc;
        Param *params = parse_paramlist(p, &pc);
        Stmt  *body   = parse_block(p);
        return ast_fn(XF_TYPE_VOID, params, pc, body, loc);
    }

    /* grouped expression */
    if (t->kind == TK_LPAREN) {
        advance(p);
        Expr *e = parse_expr(p);
        expect(p, TK_RPAREN, "expected ')' after expression");
        return e;
    }

    /* spread: ...x */
    if (t->kind == TK_DOTDOTDOT) {
        advance(p);
        return ast_spread(parse_unary(p), loc);
    }

    /* map {k:v} / set {a,b} literal */
    if (t->kind == TK_LBRACE) {
        advance(p);
        if (check(p, TK_RBRACE)) { advance(p); return ast_map_lit(NULL,NULL,0,loc); }
        Expr *first = parse_expr(p);
        if (check(p, TK_COLON)) {
            advance(p); Expr *fv = parse_expr(p);
            size_t cap=4; Expr **keys=malloc(cap*sizeof(*keys)),**vals=malloc(cap*sizeof(*vals)); size_t cnt=0;
            keys[cnt]=first; vals[cnt]=fv; cnt++;
            while(match_tok(p,TK_COMMA)){if(check(p,TK_RBRACE))break;if(cnt==cap){cap*=2;keys=realloc(keys,cap*sizeof(*keys));vals=realloc(vals,cap*sizeof(*vals));}keys[cnt]=parse_expr(p);expect(p,TK_COLON,"expected ':'");vals[cnt]=parse_expr(p);cnt++;}
            expect(p,TK_RBRACE,"expected '}'"); return ast_map_lit(keys,vals,cnt,loc);
        } else {
            size_t cap=4; Expr **items=malloc(cap*sizeof(*items)); size_t cnt=0;
            items[cnt++]=first;
            while(match_tok(p,TK_COMMA)){if(check(p,TK_RBRACE))break;if(cnt==cap){cap*=2;items=realloc(items,cap*sizeof(*items));}items[cnt++]=parse_expr(p);}
            expect(p,TK_RBRACE,"expected '}'"); return ast_set_lit(items,cnt,loc);
        }
    }
    /* explicit set{...} */
    if (t->kind == TK_KW_SET && peek_at(p,1)->kind == TK_LBRACE) {
        advance(p); advance(p);
        Expr **items=NULL; size_t cnt=0,cap=0;
        if(!check(p,TK_RBRACE)){for(;;){if(cnt==cap){size_t nc=cap?cap*2:4;items=realloc(items,nc*sizeof(*items));cap=nc;}items[cnt++]=parse_expr(p);if(match_tok(p,TK_COMMA)){if(check(p,TK_RBRACE))break;continue;}break;}}
        expect(p,TK_RBRACE,"expected '}'"); return ast_set_lit(items,cnt,loc);
    }
    /* type cast: num(x) str(x) arr(x) map(x) set(x) */
    if (is_type_kw(p) && peek_at(p, 1)->kind == TK_LPAREN) {
        uint8_t to_type = parse_type(p);
        advance(p);
        Expr *operand = parse_expr(p);
        expect(p, TK_RPAREN, "expected ')' after cast expression");
        return ast_cast(to_type, operand, loc);
    }
    parser_error(p, "expected expression");
    advance(p);
    return ast_num(0, loc);
}

/* ── postfix: call, subscript, member, .state, .type, x++, x-- ── */
Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);

    for (;;) {
        Loc loc = cur(p)->loc;

        if (check(p, TK_LPAREN)) {
            size_t argc;
            Expr **args = parse_arglist(p, &argc);
            e = ast_call(e, args, argc, loc);
            continue;
        }
        if (match_tok(p, TK_LBRACKET)) {
            Expr *key = parse_expr(p);
            expect(p, TK_RBRACKET, "expected ']' after subscript");
            e = ast_subscript(e, key, loc);
            continue;
        }
        if (check(p, TK_DOT_STATE)) {
            advance(p);
            e = ast_state(e, loc);
            continue;
        }
        if (check(p, TK_DOT_TYPE)) {
            advance(p);
            e = ast_type(e, loc);
            continue;
        }
        if (check(p, TK_DOT_LEN)) {
            advance(p);
            e = ast_len(e, loc);
            continue;
        }
        if (match_tok(p, TK_DOT)) {
            /* field names may be identifiers OR keywords (e.g. core.str, core.os) */
            Token *field = cur(p);
            if (field->kind == TK_EOF || field->lexeme_len == 0) {
                parser_error(p, "expected field name after '.'");
            } else {
                advance(p);
            }
            xf_Str *fname = tok_to_str(field);
            e = ast_member(e, fname, loc);
            xf_str_release(fname);
            continue;
        }
        if (check(p, TK_PLUS_PLUS)) {
            advance(p);
            e = ast_unary(UNOP_POST_INC, e, loc);
            continue;
        }
        if (check(p, TK_MINUS_MINUS)) {
            advance(p);
            e = ast_unary(UNOP_POST_DEC, e, loc);
            continue;
        }
        /* pipe-fn:  expr |> fn */
        if (match_tok(p, TK_PIPE_GT)) {
            Expr *right = parse_unary(p);
            e = ast_pipe_fn(e, right, loc);
            continue;
        }
        break;
    }
    return e;
}

/* ── unary: -x  !x  ++x  --x ─────────────────────────────── */
Expr *parse_unary(Parser *p) {
    Loc loc = cur(p)->loc;
    if (match_tok(p, TK_MINUS))      return ast_unary(UNOP_NEG,     parse_unary(p), loc);
    if (match_tok(p, TK_BANG))       return ast_unary(UNOP_NOT,     parse_unary(p), loc);
    if (match_tok(p, TK_PLUS_PLUS))  return ast_unary(UNOP_PRE_INC, parse_unary(p), loc);
    if (match_tok(p, TK_MINUS_MINUS))return ast_unary(UNOP_PRE_DEC, parse_unary(p), loc);
    return parse_postfix(p);
}

/* ── exponentiation: right-associative ──────────────────────── */
Expr *parse_exp(Parser *p) {
    Expr *left = parse_unary(p);
    if (match_tok(p, TK_CARET)) {
        Loc loc = cur(p)->loc;
        Expr *right = parse_exp(p);    /* right-associative */
        return ast_binary(BINOP_POW, left, right, loc);
    }
    return left;
}

/* ── multiplicative: * / % .* ./ ───────────────────────────── */
Expr *parse_mul(Parser *p) {
    Expr *left = parse_exp(p);
    for (;;) {
        Loc loc = cur(p)->loc;
        if (match_tok(p, TK_STAR))      { left = ast_binary(BINOP_MUL,  left, parse_exp(p), loc); continue; }
        if (match_tok(p, TK_SLASH))     { left = ast_binary(BINOP_DIV,  left, parse_exp(p), loc); continue; }
        if (match_tok(p, TK_PERCENT))   { left = ast_binary(BINOP_MOD,  left, parse_exp(p), loc); continue; }
        if (match_tok(p, TK_DOT_STAR))  { left = ast_binary(BINOP_MMUL, left, parse_exp(p), loc); continue; }
        if (match_tok(p, TK_DOT_SLASH)) { left = ast_binary(BINOP_MDIV, left, parse_exp(p), loc); continue; }
        break;
    }
    return left;
}

/* ── additive: + - .+ .- ────────────────────────────────────── */
Expr *parse_add(Parser *p) {
    Expr *left = parse_mul(p);
    for (;;) {
        Loc loc = cur(p)->loc;
        if (match_tok(p, TK_PLUS))      { left = ast_binary(BINOP_ADD,  left, parse_mul(p), loc); continue; }
        if (match_tok(p, TK_MINUS))     { left = ast_binary(BINOP_SUB,  left, parse_mul(p), loc); continue; }
        if (match_tok(p, TK_DOT_PLUS))  { left = ast_binary(BINOP_MADD, left, parse_mul(p), loc); continue; }
        if (match_tok(p, TK_DOT_MINUS)) { left = ast_binary(BINOP_MSUB, left, parse_mul(p), loc); continue; }
        break;
    }
    return left;
}

/* ── string concat: .. ───────────────────────────────────────── */
Expr *parse_concat(Parser *p) {
    Expr *left = parse_add(p);
    for (;;) {
        Loc loc = cur(p)->loc;
        if (match_tok(p, TK_DOT_DOT)) { left = ast_binary(BINOP_CONCAT, left, parse_add(p), loc); continue; }
        break;
    }
    return left;
}

/* ── comparison: < > <= >= <=> ───────────────────────────────── */
Expr *parse_compare(Parser *p) {
    Expr *left = parse_concat(p);
    for (;;) {
        Loc loc = cur(p)->loc;
        if (match_tok(p, TK_LT))        { left = ast_binary(BINOP_LT,       left, parse_concat(p), loc); continue; }
        if (match_tok(p, TK_GT))        { left = ast_binary(BINOP_GT,       left, parse_concat(p), loc); continue; }
        if (match_tok(p, TK_LT_EQ))     { left = ast_binary(BINOP_LTE,      left, parse_concat(p), loc); continue; }
        if (match_tok(p, TK_GT_EQ))     { left = ast_binary(BINOP_GTE,      left, parse_concat(p), loc); continue; }
        if (match_tok(p, TK_SPACESHIP)) { left = ast_binary(BINOP_SPACESHIP, left, parse_concat(p), loc); continue; }
        break;
    }
    return left;
}

/* ── equality: == != ─────────────────────────────────────────── */
Expr *parse_equality(Parser *p) {
    Expr *left = parse_compare(p);
    for (;;) {
        Loc loc = cur(p)->loc;
        if (match_tok(p, TK_EQ_EQ))   { left = ast_binary(BINOP_EQ,  left, parse_compare(p), loc); continue; }
        if (match_tok(p, TK_BANG_EQ)) { left = ast_binary(BINOP_NEQ, left, parse_compare(p), loc); continue; }
        break;
    }
    return left;
}

/* ── regex match: ~ !~ ───────────────────────────────────────── */
Expr *parse_match(Parser *p) {
    Expr *left = parse_equality(p);
    for (;;) {
        Loc loc = cur(p)->loc;
        if (match_tok(p, TK_TILDE))       { left = ast_binary(BINOP_MATCH,  left, parse_equality(p), loc); continue; }
        if (match_tok(p, TK_BANG_TILDE))  { left = ast_binary(BINOP_NMATCH, left, parse_equality(p), loc); continue; }
        break;
    }
    return left;
}

/* ── logical and: && ─────────────────────────────────────────── */
Expr *parse_and(Parser *p) {
    Expr *left = parse_match(p);
    for (;;) {
        Loc loc = cur(p)->loc;
        if (match_tok(p, TK_AMP_AMP)) { left = ast_binary(BINOP_AND, left, parse_match(p), loc); continue; }
        break;
    }
    return left;
}

/* ── logical or: || and pipe to command: | ──────────────────── */
Expr *parse_or(Parser *p) {
    Expr *left = parse_and(p);
    for (;;) {
        Loc loc = cur(p)->loc;
        if (match_tok(p, TK_PIPE_PIPE)) { left = ast_binary(BINOP_OR,       left, parse_and(p), loc); continue; }
        if (match_tok(p, TK_PIPE))      { left = ast_binary(BINOP_PIPE_CMD, left, parse_and(p), loc); continue; }
        break;
    }
    return left;
}

/* ── null coalescing: ?? ─────────────────────────────────────── */
Expr *parse_coalesce(Parser *p) {
    Expr *left = parse_or(p);
    if (check(p, TK_COALESCE)) {
        Loc loc = cur(p)->loc;
        advance(p);
        Expr *right = parse_coalesce(p);   /* right-assoc */
        return ast_coalesce(left, right, loc);
    }
    return left;
}

/* ── ternary: cond ? then : else ────────────────────────────── */
Expr *parse_ternary(Parser *p) {
    Expr *cond = parse_coalesce(p);
    if (match_tok(p, TK_QUESTION)) {
        Loc loc = cur(p)->loc;
        Expr *then = parse_expr(p);
        expect(p, TK_COLON, "expected ':' in ternary expression");
        Expr *els = parse_ternary(p);   /* right-assoc */
        return ast_ternary(cond, then, els, loc);
    }
    return cond;
}

/* ── assignment: = += -= *= /= %= ..= := ───────────────────── */
Expr *parse_assign(Parser *p) {
    Expr *left = parse_ternary(p);
    Loc   loc  = cur(p)->loc;

    /* walrus := — declare + assign in expression position */
    if (left->kind == EXPR_IDENT && check(p, TK_WALRUS)) {
        advance(p);
        xf_Str *name = xf_str_retain(left->as.ident.name);
        Expr *val = parse_assign(p);
        ast_expr_free(left);
        Expr *e = ast_walrus(name, XF_TYPE_VOID, val, loc);
        xf_str_release(name);
        return e;
    }

    AssignOp op;
    switch (cur_kind(p)) {
        case TK_EQ:          op = ASSIGNOP_EQ;     break;
        case TK_PLUS_EQ:     op = ASSIGNOP_ADD;    break;
        case TK_MINUS_EQ:    op = ASSIGNOP_SUB;    break;
        case TK_STAR_EQ:     op = ASSIGNOP_MUL;    break;
        case TK_SLASH_EQ:    op = ASSIGNOP_DIV;    break;
        case TK_PERCENT_EQ:  op = ASSIGNOP_MOD;    break;
        case TK_DOT_DOT_EQ:  op = ASSIGNOP_CONCAT; break;
        default: return left;
    }
    advance(p);
    Expr *value = parse_assign(p);    /* right-assoc */
    return ast_assign(op, left, value, loc);
}

Expr *parse_expr(Parser *p) { return parse_assign(p); }


/* ============================================================
 * Statement parsing
 * ============================================================ */

/* { stmt* } */
Stmt *parse_block(Parser *p) {
    Loc loc = cur(p)->loc;
    expect(p, TK_LBRACE, "expected '{'");

    size_t cap = 8, count = 0;
    Stmt **stmts = malloc(sizeof(Stmt *) * cap);

    sym_push(p->syms, SCOPE_BLOCK);

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        Stmt *s = parse_stmt(p);
        if (!s) continue;
        if (count >= cap) { cap *= 2; stmts = realloc(stmts, sizeof(Stmt*)*cap); }
        stmts[count++] = s;
        if (p->had_error && p->panic_mode) synchronize(p);
    }

    sym_pop(p->syms);
    expect(p, TK_RBRACE, "expected '}'");
    return ast_block(stmts, count, loc);
}

/* if (cond) { } elif (cond) { } else { } */
Stmt *parse_if(Parser *p) {
    Loc loc = cur(p)->loc;
    advance(p);   /* consume 'if' */

    size_t   cap      = 4, count = 0;
    Branch  *branches = malloc(sizeof(Branch) * cap);

    /* if branch */
    expect(p, TK_LPAREN, "expected '(' after 'if'");
    branches[count].cond = parse_expr(p);
    branches[count].loc  = loc;
    expect(p, TK_RPAREN, "expected ')' after condition");
    branches[count].body = parse_block(p);
    count++;

    /* elif branches */
    while (check(p, TK_KW_ELIF)) {
        if (count >= cap) { cap *= 2; branches = realloc(branches, sizeof(Branch)*cap); }
        Loc elif_loc = cur(p)->loc;
        advance(p);
        expect(p, TK_LPAREN, "expected '(' after 'elif'");
        branches[count].cond = parse_expr(p);
        branches[count].loc  = elif_loc;
        expect(p, TK_RPAREN, "expected ')' after condition");
        branches[count].body = parse_block(p);
        count++;
    }

    /* else */
    Stmt *els = NULL;
    if (match_tok(p, TK_KW_ELSE))
        els = parse_block(p);

    return ast_if(branches, count, els, loc);
}

/* while (cond) { body } */
Stmt *parse_while(Parser *p) {
    Loc loc = cur(p)->loc;
    advance(p);
    expect(p, TK_LPAREN, "expected '(' after 'while'");
    Expr *cond = parse_expr(p);
    expect(p, TK_RPAREN, "expected ')' after condition");
    sym_push(p->syms, SCOPE_LOOP);
    Stmt *body = parse_block(p);
    sym_pop(p->syms);
    return ast_while(cond, body, loc);
}

/* for (iter in collection) { body } */
Stmt *parse_for(Parser *p) {
    Loc loc = cur(p)->loc;
    advance(p);
    expect(p, TK_LPAREN, "expected '(' after 'for'");
    Token *iter_tok = expect(p, TK_IDENT, "expected iterator name");
    xf_Str *iter = tok_to_str(iter_tok);
    expect(p, TK_KW_IN, "expected 'in' after iterator");
    Expr *collection = parse_expr(p);
    expect(p, TK_RPAREN, "expected ')' after collection");
    sym_push(p->syms, SCOPE_LOOP);
    sym_declare(p->syms, iter, SYM_VAR, XF_TYPE_VOID, loc);
    Stmt *body = parse_block(p);
    sym_pop(p->syms);
    Stmt *s = ast_for(iter, collection, body, loc);
    xf_str_release(iter);
    return s;
}

/* return expr? ; */
Stmt *parse_return(Parser *p) {
    Loc loc = cur(p)->loc;
    advance(p);
    Expr *value = NULL;
    if (!check(p, TK_SEMICOLON) && !check(p, TK_NEWLINE) &&
        !check(p, TK_RBRACE)   && !check(p, TK_EOF))
        value = parse_expr(p);
    match_tok(p, TK_SEMICOLON);
    return ast_return(value, loc);
}

/* print expr, expr, ...  > redirect? ; */
Stmt *parse_print(Parser *p) {
    Loc loc = cur(p)->loc;
    advance(p);   /* consume 'print' */

    size_t cap = 4, count = 0;
    Expr **args = malloc(sizeof(Expr *) * cap);

    while (!check(p, TK_SEMICOLON) && !check(p, TK_NEWLINE) &&
           !check(p, TK_GT)        && !check(p, TK_GT_GT)   &&
           !check(p, TK_PIPE)      && !check(p, TK_EOF)) {
        if (count >= cap) { cap *= 2; args = realloc(args, sizeof(Expr*)*cap); }
        args[count++] = parse_expr(p);
        if (!match_tok(p, TK_COMMA)) break;
    }

    /* optional redirect */
    Expr *redirect = NULL;
    if (check(p, TK_GT) || check(p, TK_GT_GT) || check(p, TK_PIPE)) {
        advance(p);
        redirect = parse_expr(p);
    }

    match_tok(p, TK_SEMICOLON);
    return ast_print(args, count, redirect, loc);
}

/* printf fmt, arg, ... ; */
Stmt *parse_printf(Parser *p) {
    Loc loc = cur(p)->loc;
    advance(p);   /* consume 'printf' */

    size_t cap = 4, count = 0;
    Expr **args = malloc(sizeof(Expr *) * cap);

    while (!check(p, TK_SEMICOLON) && !check(p, TK_NEWLINE) &&
           !check(p, TK_GT)        && !check(p, TK_GT_GT)   &&
           !check(p, TK_PIPE)      && !check(p, TK_EOF)) {
        if (count >= cap) { cap *= 2; args = realloc(args, sizeof(Expr*)*cap); }
        args[count++] = parse_expr(p);
        if (!match_tok(p, TK_COMMA)) break;
    }

    Expr *redirect = NULL;
    if (check(p, TK_GT) || check(p, TK_GT_GT) || check(p, TK_PIPE)) {
        advance(p);
        redirect = parse_expr(p);
    }

    match_tok(p, TK_SEMICOLON);
    return ast_printf_stmt(args, count, redirect, loc);
}

/* outfmt "csv"|"tsv"|"json"|"text" ; */
Stmt *parse_outfmt(Parser *p) {
    Loc loc = cur(p)->loc;
    advance(p);   /* consume 'outfmt' */

    uint8_t mode = XF_OUTFMT_TEXT;
    if (check(p, TK_STR)) {
        Token *t = advance(p);
        if      (strncmp(t->val.str.data, "csv",  3) == 0) mode = XF_OUTFMT_CSV;
        else if (strncmp(t->val.str.data, "tsv",  3) == 0) mode = XF_OUTFMT_TSV;
        else if (strncmp(t->val.str.data, "json", 4) == 0) mode = XF_OUTFMT_JSON;
        else if (strncmp(t->val.str.data, "text", 4) == 0) mode = XF_OUTFMT_TEXT;
        else parser_warning(p, "unknown outfmt mode; defaulting to 'text'");
    } else {
        parser_error(p, "expected string after 'outfmt'");
    }

    match_tok(p, TK_SEMICOLON);
    return ast_outfmt(mode, loc);
}

/* spawn call ; */
Stmt *parse_spawn(Parser *p) {
    Loc loc = cur(p)->loc;
    advance(p);
    Expr *call = parse_expr(p);
    match_tok(p, TK_SEMICOLON);
    return ast_spawn(call, loc);
}

/* join handle ; */
Stmt *parse_join(Parser *p) {
    Loc loc = cur(p)->loc;
    advance(p);
    Expr *handle = parse_expr(p);
    match_tok(p, TK_SEMICOLON);
    return ast_join(handle, loc);
}

/* import "path" ; */
Stmt *parse_import(Parser *p) {
    Loc loc = cur(p)->loc;
    advance(p);
    Token *path_tok = expect(p, TK_STR, "expected string path after 'import'");
    xf_Str *path = xf_str_new(path_tok->val.str.data, path_tok->val.str.len);
    match_tok(p, TK_SEMICOLON);
    Stmt *s = ast_import(path, loc);
    xf_str_release(path);
    return s;
}

/* type fn name(params) { body } — as a statement inside a block */
Stmt *parse_fn_decl(Parser *p) {
    Loc loc = cur(p)->loc;
    uint8_t ret = parse_type(p);
    advance(p);   /* consume 'fn' */
    Token *name_tok = expect(p, TK_IDENT, "expected function name");
    xf_Str *name = tok_to_str(name_tok);

    size_t pc;
    Param *params = parse_paramlist(p, &pc);

    /* register in current scope before parsing body (allows recursion) */
    sym_declare(p->syms, name, SYM_FN, XF_TYPE_FN, loc);

    Scope *fn_sc = sym_push(p->syms, SCOPE_FN);
    fn_sc->fn_ret_type = ret;

    /* declare params in fn scope */
    for (size_t i = 0; i < pc; i++)
        sym_declare(p->syms, params[i].name, SYM_PARAM, params[i].type, params[i].loc);

    Stmt *body = parse_block(p);
    sym_pop(p->syms);

    Stmt *s = ast_fn_decl(ret, name, params, pc, body, loc);
    xf_str_release(name);
    return s;
}

/* type name = expr? ; — variable declaration */
Stmt *parse_var_decl(Parser *p, uint8_t type) {
    Loc     loc  = cur(p)->loc;
    Token  *name = expect(p, TK_IDENT, "expected variable name");
    xf_Str *n    = tok_to_str(name);

    Expr *init = NULL;
    if (match_tok(p, TK_EQ)) {
        if (type == XF_TYPE_SET && check(p, TK_LBRACE)) {
            Loc sloc = cur(p)->loc; advance(p);
            Expr **items = NULL; size_t count=0, cap=0;
            if (!check(p, TK_RBRACE)) {
                for(;;){if(count==cap){size_t nc=cap?cap*2:4;items=realloc(items,nc*sizeof(*items));cap=nc;}items[count++]=parse_expr(p);if(match_tok(p,TK_COMMA)){if(check(p,TK_RBRACE))break;continue;}break;}
            }
            expect(p, TK_RBRACE, "expected '}' after set literal");
            init = ast_set_lit(items, count, sloc);
        } else { init = parse_expr(p); }
    }

    match_tok(p, TK_SEMICOLON);

    sym_declare(p->syms, n, SYM_VAR, type, loc);
    Stmt *s = ast_var_decl(type, n, init, loc);
    xf_str_release(n);
    return s;
}

Stmt *parse_stmt(Parser *p) {
    Loc loc = cur(p)->loc;

    /* skip stray newlines / semicolons */
    while (check(p, TK_NEWLINE) || check(p, TK_SEMICOLON)) advance(p);
    if (check(p, TK_EOF)) return NULL;

    /* block */
    if (check(p, TK_LBRACE)) return parse_block(p);

    /* fn declaration: type fn name ... */
    if (is_type_kw(p) && peek_at(p, 1)->kind == TK_KW_FN)
        return parse_fn_decl(p);

    /* variable declaration: type name */
    if (is_type_kw(p) && peek_at(p, 1)->kind == TK_IDENT) {
        uint8_t type = parse_type(p);
        return parse_var_decl(p, type);
    }

    /* type keyword before a built-in ivar (e.g. "str FS = ","") is a
     * common mistake — FS/RS/OFS/ORS are built-ins, not user variables.
     * Consume the type keyword and parse the rest as an expression so
     * the ivar assignment still takes effect at runtime. */
    if (is_type_kw(p) && (peek_at(p, 1)->kind == TK_VAR_FS  ||
                           peek_at(p, 1)->kind == TK_VAR_RS  ||
                           peek_at(p, 1)->kind == TK_VAR_OFS ||
                           peek_at(p, 1)->kind == TK_VAR_ORS ||
                           peek_at(p, 1)->kind == TK_VAR_NR  ||
                           peek_at(p, 1)->kind == TK_VAR_NF  ||
                           peek_at(p, 1)->kind == TK_VAR_FNR)) {
        parser_warning(p, "type keyword before built-in variable ignored; "
                          "did you mean a bare assignment?");
        advance(p);   /* discard the type keyword */
        /* fall through to expr-stmt below */
    }

    /* control flow */
    if (check(p, TK_KW_IF))     return parse_if(p);
    if (check(p, TK_KW_WHILE))  return parse_while(p);
    if (check(p, TK_KW_FOR))    return parse_for(p);
    if (check(p, TK_KW_RETURN)) return parse_return(p);
    if (check(p, TK_KW_PRINT))   return parse_print(p);
    if (check(p, TK_KW_PRINTF))  return parse_printf(p);
    if (check(p, TK_KW_OUTFMT))  return parse_outfmt(p);
    if (check(p, TK_KW_SPAWN))  return parse_spawn(p);
    if (check(p, TK_KW_JOIN))   return parse_join(p);
    if (check(p, TK_KW_IMPORT)) return parse_import(p);

    if (check(p, TK_KW_NEXT)) {
        advance(p); match_tok(p, TK_SEMICOLON);
        return ast_next(loc);
    }
    if (check(p, TK_KW_EXIT)) {
        advance(p); match_tok(p, TK_SEMICOLON);
        return ast_exit(loc);
    }
    if (check(p, TK_KW_DELETE)) {
        advance(p);
        Expr *target = parse_expr(p);
        match_tok(p, TK_SEMICOLON);
        return ast_delete(target, loc);
    }

    /* s/pat/rep/flags */
    if (check(p, TK_SUBST)) {
        Token *t = advance(p);
        xf_Str *pat = xf_str_from_cstr(t->val.re.pattern);
        xf_Str *rep = xf_str_from_cstr(t->val.re.replacement);
        Stmt *s = ast_subst(pat, rep, t->val.re.flags, NULL, loc);
        xf_str_release(pat); xf_str_release(rep);
        match_tok(p, TK_SEMICOLON);
        return s;
    }

    /* y/from/to/ */
    if (check(p, TK_TRANS)) {
        Token *t = advance(p);
        xf_Str *from = xf_str_from_cstr(t->val.trans.from);
        xf_Str *to   = xf_str_from_cstr(t->val.trans.to);
        Stmt *s = ast_trans(from, to, NULL, loc);
        xf_str_release(from); xf_str_release(to);
        match_tok(p, TK_SEMICOLON);
        return s;
    }

    /*
     * Shorthand while:   expr <> stmt ;
     * Shorthand for:     expr[ident] > stmt ;
     * Detected after parsing the left expression.
     */
    Expr *expr = parse_expr(p);

    /* cond <> body — shorthand while */
    if (match_tok(p, TK_DIAMOND)) {
        Stmt *body = check(p, TK_LBRACE)
                         ? parse_block(p)
                         : parse_stmt(p);
        match_tok(p, TK_SEMICOLON);
        return ast_while_short(expr, body, loc);
    }

    /* collection[iter] > body — shorthand for */
    if (expr->kind == EXPR_SUBSCRIPT && check(p, TK_GT) &&
        expr->as.subscript.key->kind == EXPR_IDENT) {
        advance(p);   /* consume > */
        xf_Str *iter = xf_str_retain(expr->as.subscript.key->as.ident.name);
        Expr *obj = expr->as.subscript.obj;
        expr->as.subscript.obj = NULL;
        ast_expr_free(expr);

        sym_push(p->syms, SCOPE_LOOP);
        sym_declare(p->syms, iter, SYM_VAR, XF_TYPE_VOID, loc);
        Stmt *body = check(p, TK_LBRACE) ? parse_block(p) : parse_stmt(p);
        sym_pop(p->syms);

        Stmt *s = ast_for_short(obj, iter, body, loc);
        xf_str_release(iter);
        match_tok(p, TK_SEMICOLON);
        return s;
    }

    match_tok(p, TK_SEMICOLON);
    return ast_expr_stmt(expr, loc);
}


/* ============================================================
 * Top-level parsing
 * ============================================================ */

/* type fn name(params) { body } — at top level */
TopLevel *parse_fn_decl_top(Parser *p) {
    Loc loc = cur(p)->loc;
    uint8_t ret = parse_type(p);
    advance(p);   /* consume 'fn' */
    Token *name_tok = expect(p, TK_IDENT, "expected function name");
    xf_Str *name = tok_to_str(name_tok);

    size_t pc;
    Param *params = parse_paramlist(p, &pc);

    sym_declare(p->syms, name, SYM_FN, XF_TYPE_FN, loc);

    Scope *fn_sc = sym_push(p->syms, SCOPE_FN);
    fn_sc->fn_ret_type = ret;
    for (size_t i = 0; i < pc; i++)
        sym_declare(p->syms, params[i].name, SYM_PARAM, params[i].type, params[i].loc);

    Stmt *body = parse_block(p);
    sym_pop(p->syms);

    TopLevel *t = ast_top_fn(ret, name, params, pc, body, loc);
    xf_str_release(name);
    return t;
}

/* pattern-action rule, bare { body }, or top-level statement */
TopLevel *parse_rule(Parser *p) {
    Loc loc = cur(p)->loc;

    /* bare action: { body } — no pattern */
    if (check(p, TK_LBRACE)) {
        sym_push(p->syms, SCOPE_PATTERN);
        Stmt *body = parse_block(p);
        sym_pop(p->syms);
        return ast_top_rule(NULL, body, loc);
    }

    /* statement-starting keywords that can never be a pattern expression —
       route directly through parse_stmt and wrap as TOP_STMT */
    if (is_type_kw(p)              ||
        check(p, TK_KW_IF)        ||
        check(p, TK_KW_WHILE)     ||
        check(p, TK_KW_FOR)       ||
        check(p, TK_KW_PRINT)     ||
        check(p, TK_KW_PRINTF)    ||
        check(p, TK_KW_OUTFMT)    ||
        check(p, TK_KW_RETURN)    ||
        check(p, TK_KW_SPAWN)     ||
        check(p, TK_KW_JOIN)      ||
        check(p, TK_KW_NEXT)      ||
        check(p, TK_KW_EXIT)      ||
        check(p, TK_KW_DELETE)    ||
        check(p, TK_SUBST)        ||
        check(p, TK_TRANS))
    {
        Stmt *s = parse_stmt(p);
        return ast_top_stmt(s, loc);
    }

    /* expression — could be:
     *   expr { }      → pattern-action rule
     *   expr <> stmt  → shorthand while  (TOP_STMT)
     *   expr[k] > s   → shorthand for    (TOP_STMT)
     *   expr ;        → bare expression  (TOP_STMT)
     * Parse the expression first, then dispatch on what follows. */
    Expr *pattern = parse_expr(p);

    /* pattern { body } — true pattern-action rule */
    if (check(p, TK_LBRACE)) {
        sym_push(p->syms, SCOPE_PATTERN);
        Stmt *body = parse_block(p);
        sym_pop(p->syms);
        return ast_top_rule(pattern, body, loc);
    }

    /* expr <> body — shorthand while */
    if (match_tok(p, TK_DIAMOND)) {
        Stmt *body = check(p, TK_LBRACE) ? parse_block(p) : parse_stmt(p);
        match_tok(p, TK_SEMICOLON);
        return ast_top_stmt(ast_while_short(pattern, body, loc), loc);
    }

    /* collection[iter] > body — shorthand for */
    if (pattern->kind == EXPR_SUBSCRIPT && check(p, TK_GT) &&
        pattern->as.subscript.key->kind == EXPR_IDENT) {
        advance(p);   /* consume > */
        xf_Str *iter = xf_str_retain(pattern->as.subscript.key->as.ident.name);
        Expr *obj = pattern->as.subscript.obj;
        pattern->as.subscript.obj = NULL;
        ast_expr_free(pattern);

        sym_push(p->syms, SCOPE_LOOP);
        sym_declare(p->syms, iter, SYM_VAR, XF_TYPE_VOID, loc);
        Stmt *body = check(p, TK_LBRACE) ? parse_block(p) : parse_stmt(p);
        sym_pop(p->syms);

        Stmt *s = ast_for_short(obj, iter, body, loc);
        xf_str_release(iter);
        match_tok(p, TK_SEMICOLON);
        return ast_top_stmt(s, loc);
    }

    /* bare expression statement */
    match_tok(p, TK_SEMICOLON);
    return ast_top_stmt(ast_expr_stmt(pattern, loc), loc);
}

TopLevel *parse_top(Parser *p) {
    while (check(p, TK_NEWLINE) || check(p, TK_SEMICOLON)) advance(p);
    if (check(p, TK_EOF)) return NULL;

    Loc loc = cur(p)->loc;

    /* BEGIN { } */
    if (check(p, TK_KW_BEGIN)) {
        advance(p);
        Stmt *body = parse_block(p);
        return ast_top_begin(body, loc);
    }

    /* END { } */
    if (check(p, TK_KW_END)) {
        advance(p);
        Stmt *body = parse_block(p);
        return ast_top_end(body, loc);
    }

    /* function declaration: type fn name ... */
    if (is_type_kw(p) && peek_at(p, 1)->kind == TK_KW_FN)
        return parse_fn_decl_top(p);

    /* import at top level */
    if (check(p, TK_KW_IMPORT)) {
        Stmt *s = parse_import(p);
        return ast_top_stmt(s, loc);
    }

    /* pattern-action rule or bare action */
    return parse_rule(p);
}


/* ============================================================
 * Public entry points
 * ============================================================ */

Program *parse(Lexer *lex, SymTable *syms) {
    Parser p = {0};
    p.lex  = lex;
    p.syms = syms;
    p.pos  = 0;

    Program *prog = ast_program_new(lex->source_name);

    while (!check(&p, TK_EOF)) {
        TopLevel *item = parse_top(&p);
        if (item) ast_program_push(prog, item);
        if (p.had_error && p.panic_mode) synchronize(&p);
    }

    return prog;
}

TopLevel *parse_repl_line(Lexer *lex, SymTable *syms) {
    Parser p = {0};
    p.lex  = lex;
    p.syms = syms;
    p.pos  = 0;

    while (check(&p, TK_NEWLINE)) advance(&p);
    if (check(&p, TK_EOF)) return NULL;

    return parse_top(&p);
}

Expr *parse_expr_str(const char *src, SymTable *syms) {
    Lexer lex;
    xf_lex_init_cstr(&lex, src, XF_SRC_INLINE, "<expr>");
    xf_tokenize(&lex);

    Parser p = {0};
    p.lex  = &lex;
    p.syms = syms;

    Expr *e = parse_expr(&p);
    xf_lex_free(&lex);
    return e;
}