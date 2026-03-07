#include "../include/interp.h"
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
static inline xf_Value propagate_err(xf_Value v) { return v; }
void interp_init(Interp *it, SymTable *syms, VM *vm) {
    memset(it, 0, sizeof(*it));
    it->syms = syms;
    it->vm   = vm;
}
void interp_free(Interp *it) { (void)it; }
void interp_error(Interp *it, Loc loc, const char *fmt, ...) {
    it->had_error = true;
    va_list ap; va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(it->err_msg, sizeof(it->err_msg), "%s", msg);
    fprintf(stdout, "ERR ──────────────────────────────────────\r\n");
    fprintf(stdout, "  %s\r\n", msg);
    fprintf(stdout, "  at %s:%u:%u\r\n", loc.source, loc.line, loc.col);
    fprintf(stdout, "──────────────────────────────────────────\r\n");
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
            if (sv.state == XF_STATE_OK && sv.data.str) {
                fputs(sv.data.str->data, out);
            } else {
                fputs(XF_TYPE_NAMES[v.type], out);
            }
            return;
        }
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
    if (v.type  == XF_TYPE_NUM) return v.data.num != 0.0;
    if (v.type  == XF_TYPE_STR) return v.data.str && v.data.str->len > 0;
    return true;
}
static xf_Value make_bool(bool b) {
    return xf_val_ok_num(b ? 1.0 : 0.0);
}
static int val_cmp(xf_Value a, xf_Value b) {
    xf_Value na = xf_coerce_num(a), nb = xf_coerce_num(b);
    if (na.state == XF_STATE_OK && nb.state == XF_STATE_OK) {
        if (na.data.num < nb.data.num) return -1;
        if (na.data.num > nb.data.num) return  1;
        return 0;
    }
    xf_Value sa = xf_coerce_str(a), sb = xf_coerce_str(b);
    if (sa.state == XF_STATE_OK && sb.state == XF_STATE_OK)
        return strcmp(sa.data.str->data, sb.data.str->data);
    return 0;
}
static xf_Value val_concat(xf_Value a, xf_Value b) {
    xf_Value sa = xf_coerce_str(a), sb = xf_coerce_str(b);
    if (sa.state != XF_STATE_OK) return sa;
    if (sb.state != XF_STATE_OK) return sb;
    size_t la = sa.data.str->len, lb = sb.data.str->len;
    char *buf = malloc(la + lb + 1);
    memcpy(buf, sa.data.str->data, la);
    memcpy(buf + la, sb.data.str->data, lb);
    buf[la+lb] = '\0';
    xf_Str *s = xf_str_new(buf, la+lb);
    free(buf);
    xf_Value r = xf_val_ok_str(s);
    xf_str_release(s);
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
        switch (target->as.ivar.var) {
            case TK_VAR_FS:  strncpy(vm->rec.fs,  s, sizeof(vm->rec.fs)-1);  return true;
            case TK_VAR_RS:  strncpy(vm->rec.rs,  s, sizeof(vm->rec.rs)-1);  return true;
            case TK_VAR_OFS: strncpy(vm->rec.ofs, s, sizeof(vm->rec.ofs)-1); return true;
            case TK_VAR_ORS: strncpy(vm->rec.ors, s, sizeof(vm->rec.ors)-1); return true;
            default: break;
        }
    }
    if (target->kind == EXPR_SUBSCRIPT) {
        Expr *obj_expr = target->as.subscript.obj;
        xf_Value key   = interp_eval_expr(it, target->as.subscript.key);
        if (key.state != XF_STATE_OK) {
            interp_error(it, target->loc, "subscript key is not OK");
            return false;
        }
        xf_Value container = lvalue_load(it, obj_expr);
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
                interp_error(it, target->loc, "map key must be a string");
                return false;
            }
            xf_map_set(container.data.map, sk.data.str, val);
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
        return s->value;
    }
    return interp_eval_expr(it, target);
}
static size_t xf_sprintf_impl(char *out, size_t cap,
                               const char *fmt,
                               xf_Value *args, size_t argc) {
    size_t wi = 0;
    size_t ai = 1;
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
    VM *vm = it->vm;
    switch (mode) {
        case XF_OUTFMT_CSV:
            for (size_t i = 0; i < count; i++) {
                if (i > 0) fputc(',', stdout);
                xf_Value sv = xf_coerce_str(vals[i]);
                csv_quote(stdout, (sv.state==XF_STATE_OK&&sv.data.str)
                                  ? sv.data.str->data : "");
            }
            fputs(vm->rec.ors, stdout);
            break;
        case XF_OUTFMT_TSV:
            for (size_t i = 0; i < count; i++) {
                if (i > 0) fputc('\t', stdout);
                xf_Value sv = xf_coerce_str(vals[i]);
                tsv_escape(stdout, (sv.state==XF_STATE_OK&&sv.data.str)
                                   ? sv.data.str->data : "");
            }
            fputs(vm->rec.ors, stdout);
            break;
        case XF_OUTFMT_JSON: {
            fputc('{', stdout);
            for (size_t i = 0; i < count; i++) {
                if (i > 0) fputc(',', stdout);
                if (i < vm->rec.header_count)
                    json_str(stdout, vm->rec.headers[i]);
                else {
                    char key[32]; snprintf(key, sizeof(key), "f%zu", i+1);
                    json_str(stdout, key);
                }
                fputc(':', stdout);
                xf_Value sv = xf_coerce_str(vals[i]);
                json_str(stdout, (sv.state==XF_STATE_OK&&sv.data.str)
                                 ? sv.data.str->data : "");
            }
            fputs("}", stdout);
            fputs(vm->rec.ors, stdout);
            break;
        }
        default:
            for (size_t i = 0; i < count; i++) {
                if (i > 0) fputs(vm->rec.ofs, stdout);
                xf_Value sv = xf_coerce_str(vals[i]);
                if (sv.state==XF_STATE_OK && sv.data.str)
                    fputs(sv.data.str->data, stdout);
                else
                    fputs(XF_STATE_NAMES[vals[i].state], stdout);
            }
            fputs(vm->rec.ors, stdout);
            break;
    }
}
xf_Value interp_call_builtin(Interp *it, const char *name,
                              xf_Value *args, size_t argc) {
    (void)it;
    if (strcmp(name,"sin")  ==0 && argc==1) { xf_Value n=xf_coerce_num(args[0]); return n.state==XF_STATE_OK?xf_val_ok_num(sin(n.data.num)):n; }
    if (strcmp(name,"cos")  ==0 && argc==1) { xf_Value n=xf_coerce_num(args[0]); return n.state==XF_STATE_OK?xf_val_ok_num(cos(n.data.num)):n; }
    if (strcmp(name,"sqrt") ==0 && argc==1) { xf_Value n=xf_coerce_num(args[0]); return n.state==XF_STATE_OK?xf_val_ok_num(sqrt(n.data.num)):n; }
    if (strcmp(name,"abs")  ==0 && argc==1) { xf_Value n=xf_coerce_num(args[0]); return n.state==XF_STATE_OK?xf_val_ok_num(fabs(n.data.num)):n; }
    if (strcmp(name,"int")  ==0 && argc==1) { xf_Value n=xf_coerce_num(args[0]); return n.state==XF_STATE_OK?xf_val_ok_num(trunc(n.data.num)):n; }
    if (strcmp(name,"rand") ==0)            { return xf_val_ok_num((double)rand()/(double)RAND_MAX); }
    if (strcmp(name,"srand")==0 && argc==1) { xf_Value n=xf_coerce_num(args[0]); if(n.state==XF_STATE_OK)srand((unsigned)n.data.num); return xf_val_null(); }
    if (strcmp(name,"len")==0 && argc==1) {
        xf_Value s = xf_coerce_str(args[0]);
        if (s.state != XF_STATE_OK) return s;
        return xf_val_ok_num((double)s.data.str->len);
    }
    if ((strcmp(name,"toupper")==0 || strcmp(name,"upper")==0) && argc==1) {
        xf_Value s = xf_coerce_str(args[0]);
        if (s.state != XF_STATE_OK) return s;
        char *buf = strdup(s.data.str->data);
        for (char *p=buf; *p; p++) *p = (char)toupper((unsigned char)*p);
        xf_Str *r = xf_str_from_cstr(buf); free(buf);
        xf_Value v = xf_val_ok_str(r); xf_str_release(r); return v;
    }
    if ((strcmp(name,"tolower")==0 || strcmp(name,"lower")==0) && argc==1) {
        xf_Value s = xf_coerce_str(args[0]);
        if (s.state != XF_STATE_OK) return s;
        char *buf = strdup(s.data.str->data);
        for (char *p=buf; *p; p++) *p = (char)tolower((unsigned char)*p);
        xf_Str *r = xf_str_from_cstr(buf); free(buf);
        xf_Value v = xf_val_ok_str(r); xf_str_release(r); return v;
    }
    if (strcmp(name,"trim")==0 && argc==1) {
        xf_Value s = xf_coerce_str(args[0]);
        if (s.state != XF_STATE_OK) return s;
        const char *p = s.data.str->data;
        while (*p && isspace((unsigned char)*p)) p++;
        size_t len = strlen(p);
        while (len > 0 && isspace((unsigned char)p[len-1])) len--;
        xf_Str *r = xf_str_new(p, len);
        xf_Value v = xf_val_ok_str(r); xf_str_release(r); return v;
    }
    if (strcmp(name,"substr")==0 && argc>=2) {
        xf_Value s = xf_coerce_str(args[0]);
        xf_Value n = xf_coerce_num(args[1]);
        if (s.state!=XF_STATE_OK) return s;
        if (n.state!=XF_STATE_OK) return n;
        size_t start = (size_t)n.data.num;
        if (start > 0) start--;
        size_t slen = s.data.str->len;
        if (start >= slen) { xf_Str *r=xf_str_from_cstr(""); xf_Value v=xf_val_ok_str(r); xf_str_release(r); return v; }
        size_t take = slen - start;
        if (argc >= 3) {
            xf_Value l = xf_coerce_num(args[2]);
            if (l.state==XF_STATE_OK) take = (size_t)l.data.num;
        }
        if (start+take > slen) take = slen-start;
        xf_Str *r = xf_str_new(s.data.str->data+start, take);
        xf_Value v = xf_val_ok_str(r); xf_str_release(r); return v;
    }
    if (strcmp(name,"index")==0 && argc==2) {
        xf_Value s=xf_coerce_str(args[0]), t=xf_coerce_str(args[1]);
        if (s.state!=XF_STATE_OK||t.state!=XF_STATE_OK) return xf_val_ok_num(0);
        char *p = strstr(s.data.str->data, t.data.str->data);
        return xf_val_ok_num(p ? (double)(p - s.data.str->data + 1) : 0.0);
    }
    if (strcmp(name,"sprintf")==0 && argc>=1) {
        xf_Value fmt = xf_coerce_str(args[0]);
        if (fmt.state != XF_STATE_OK) return fmt;
        char buf[8192];
        xf_sprintf_impl(buf, sizeof(buf), fmt.data.str->data, args, argc);
        xf_Str *r = xf_str_from_cstr(buf);
        xf_Value v = xf_val_ok_str(r); xf_str_release(r); return v;
    }
    if (strcmp(name,"column")==0 && argc>=2) {
        xf_Value sv = xf_coerce_str(args[0]);
        xf_Value wv = xf_coerce_num(args[1]);
        if (sv.state!=XF_STATE_OK || wv.state!=XF_STATE_OK)
            return propagate_err(sv.state!=XF_STATE_OK ? sv : wv);
        const char *s   = sv.data.str ? sv.data.str->data : "";
        size_t slen     = sv.data.str ? sv.data.str->len  : 0;
        int    width    = (int)wv.data.num;
        bool   right    = false;
        char   pad      = ' ';
        if (argc >= 3) {
            xf_Value av = xf_coerce_str(args[2]);
            if (av.state==XF_STATE_OK && av.data.str) {
                if (strcmp(av.data.str->data, "right")==0) right = true;
                else if (av.data.str->len == 1) pad = av.data.str->data[0];
            }
        }
        if (width <= 0 || (int)slen >= width) {
            xf_Str *r = xf_str_new(s, slen);
            xf_Value v = xf_val_ok_str(r); xf_str_release(r); return v;
        }
        size_t total = (size_t)width;
        char *buf = malloc(total + 1);
        size_t pad_count = total - slen;
        if (right) {
            memset(buf, pad, pad_count);
            memcpy(buf + pad_count, s, slen);
        } else {
            memcpy(buf, s, slen);
            memset(buf + slen, pad, pad_count);
        }
        buf[total] = '\0';
        xf_Str *r = xf_str_new(buf, total);
        free(buf);
        xf_Value v = xf_val_ok_str(r); xf_str_release(r); return v;
    }
    if (strcmp(name,"system")==0 && argc==1) {
        xf_Value s = xf_coerce_str(args[0]);
        if (s.state != XF_STATE_OK) return s;
        int rc = system(s.data.str->data);
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
            return coll;
        }
        return xf_val_nav(XF_TYPE_VOID);
    }
    if (strcmp(name,"has")==0 && argc==2) {
        xf_Value coll=args[0];
        if (coll.state!=XF_STATE_OK||!coll.data.map) return xf_val_ok_num(0);
        if (coll.type==XF_TYPE_MAP||coll.type==XF_TYPE_SET) {
            xf_Value ks=xf_coerce_str(args[1]);
            if (ks.state!=XF_STATE_OK||!ks.data.str) return xf_val_ok_num(0);
            xf_Value r=xf_map_get(coll.data.map,ks.data.str);
            return xf_val_ok_num(r.state!=XF_STATE_NAV?1.0:0.0);
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
        if (sv.state != XF_STATE_OK || !sv.data.str) return xf_val_nav(XF_TYPE_STR);
        bool is_pipe = true;
        FILE *fp = popen(sv.data.str->data, "r");
        if (!fp) {
            is_pipe = false;
            fp = fopen(sv.data.str->data, "r");
        }
        if (!fp) return xf_val_nav(XF_TYPE_STR);
        char buf[65536];
        size_t n = 0;
        int c;
        while (n < sizeof(buf) - 1 && (c = fgetc(fp)) != EOF) {
            buf[n++] = (char)c;
        }
        buf[n] = '\0';
        if (is_pipe) pclose(fp);
        else fclose(fp);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        xf_Str *r = xf_str_from_cstr(buf);
        xf_Value rv = xf_val_ok_str(r);
        xf_str_release(r);
        return rv;
    }
    if (strcmp(name,"lines")==0 && argc==1) {
        xf_Value sv = xf_coerce_str(args[0]);
        if (sv.state != XF_STATE_OK || !sv.data.str) return xf_val_nav(XF_TYPE_ARR);
        bool is_pipe = true;
        FILE *fp = popen(sv.data.str->data, "r");
        if (!fp) {
            is_pipe = false;
            fp = fopen(sv.data.str->data, "r");
        }
        if (!fp) return xf_val_nav(XF_TYPE_ARR);
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
        if (fv.state!=XF_STATE_OK||sv.state!=XF_STATE_OK) return xf_val_ok_num(0);
        FILE *fp=fopen(fv.data.str->data,"w"); if(!fp) return xf_val_ok_num(0);
        fwrite(sv.data.str->data,1,sv.data.str->len,fp); fclose(fp); return xf_val_ok_num(1);
    }
    if (strcmp(name,"append")==0 && argc==2) {
        xf_Value fv=xf_coerce_str(args[0]),sv=xf_coerce_str(args[1]);
        if (fv.state!=XF_STATE_OK||sv.state!=XF_STATE_OK) return xf_val_ok_num(0);
        FILE *fp=fopen(fv.data.str->data,"a"); if(!fp) return xf_val_ok_num(0);
        fwrite(sv.data.str->data,1,sv.data.str->len,fp); fclose(fp); return xf_val_ok_num(1);
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
        return xf_val_ok_map(m);
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
        xf_Value out=xf_val_ok_map(m); out.type=XF_TYPE_SET; return out;
    }
    case EXPR_IDENT: {
        Symbol *s = sym_lookup_str(it->syms, e->as.ident.name);
        if (!s) {
            interp_error(it, e->loc, "undefined variable '%s'",
                         e->as.ident.name->data);
            return xf_val_nav(XF_TYPE_VOID);
        }
        return s->value;
    }
    case EXPR_FIELD: {
        int n = e->as.field.index;
        VM *vm = it->vm;
        if (n == 0) {
            if (!vm->rec.buf) return xf_val_ok_str(xf_str_from_cstr(""));
            xf_Str *s = xf_str_new(vm->rec.buf, vm->rec.buf_len);
            xf_Value v = xf_val_ok_str(s); xf_str_release(s); return v;
        }
        if (n > 0 && (size_t)n <= vm->rec.field_count) {
            xf_Str *s = xf_str_from_cstr(vm->rec.fields[n-1]);
            xf_Value v = xf_val_ok_str(s); xf_str_release(s); return v;
        }
        return xf_val_ok_str(xf_str_from_cstr(""));
    }
    case EXPR_IVAR: {
        VM *vm = it->vm;
        switch (e->as.ivar.var) {
            case TK_VAR_NR:  return xf_val_ok_num((double)vm->rec.nr);
            case TK_VAR_NF:  return xf_val_ok_num((double)vm->rec.field_count);
            case TK_VAR_FNR: return xf_val_ok_num((double)vm->rec.fnr);
            case TK_VAR_FS:  { xf_Str *s=xf_str_from_cstr(vm->rec.fs);  xf_Value v=xf_val_ok_str(s);xf_str_release(s);return v; }
            case TK_VAR_RS:  { xf_Str *s=xf_str_from_cstr(vm->rec.rs);  xf_Value v=xf_val_ok_str(s);xf_str_release(s);return v; }
            case TK_VAR_OFS: { xf_Str *s=xf_str_from_cstr(vm->rec.ofs); xf_Value v=xf_val_ok_str(s);xf_str_release(s);return v; }
            case TK_VAR_ORS: { xf_Str *s=xf_str_from_cstr(vm->rec.ors); xf_Value v=xf_val_ok_str(s);xf_str_release(s);return v; }
            default: return xf_val_nav(XF_TYPE_VOID);
        }
    }
    case EXPR_SUBSCRIPT: {
        xf_Value obj = interp_eval_expr(it, e->as.subscript.obj);
        xf_Value key = interp_eval_expr(it, e->as.subscript.key);
        if (obj.state != XF_STATE_OK) return obj;
        if (key.state != XF_STATE_OK) return key;
        if (obj.type == XF_TYPE_ARR) {
            xf_Value ni = xf_coerce_num(key);
            if (ni.state != XF_STATE_OK || !obj.data.arr)
                return xf_val_nav(XF_TYPE_VOID);
            return xf_arr_get(obj.data.arr, (size_t)ni.data.num);
        }
        if (obj.type == XF_TYPE_MAP) {
            xf_Value sk = xf_coerce_str(key);
            if (sk.state != XF_STATE_OK || !obj.data.map)
                return xf_val_nav(XF_TYPE_VOID);
            return xf_map_get(obj.data.map, sk.data.str);
        }
        if (obj.type == XF_TYPE_STR && obj.data.str) {
            xf_Value ni = xf_coerce_num(key);
            if (ni.state != XF_STATE_OK) return xf_val_nav(XF_TYPE_STR);
            size_t idx = (size_t)ni.data.num;
            if (idx >= obj.data.str->len) return xf_val_nav(XF_TYPE_STR);
            char ch[2] = { obj.data.str->data[idx], '\0' };
            xf_Str *cs = xf_str_new(ch, 1);
            xf_Value rv = xf_val_ok_str(cs); xf_str_release(cs); return rv;
        }
        interp_error(it, e->loc, "subscript on non-indexable type '%s'",
                     XF_TYPE_NAMES[obj.type]);
        return xf_val_nav(XF_TYPE_VOID);
    }
    case EXPR_SVAR: {
        return xf_val_ok_str(xf_str_from_cstr(""));
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
            if (!is_truthy(a)) return make_bool(false);
            return make_bool(is_truthy(interp_eval_expr(it, e->as.binary.right)));
        }
        if (e->as.binary.op == BINOP_OR) {
            xf_Value a = interp_eval_expr(it, e->as.binary.left);
            if (is_truthy(a)) return make_bool(true);
            return make_bool(is_truthy(interp_eval_expr(it, e->as.binary.right)));
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
    if (cmd.state != XF_STATE_OK || !cmd.data.str)
        return xf_val_nav(XF_TYPE_STR);

    xf_Value sv = xf_coerce_str(a);
    if (sv.state != XF_STATE_OK || !sv.data.str)
        return xf_val_nav(XF_TYPE_STR);

    char tmpname[] = "/tmp/xf_pipe_XXXXXX";
    int fd = mkstemp(tmpname);
    if (fd < 0)
        return xf_val_nav(XF_TYPE_STR);

    FILE *tf = fdopen(fd, "w");
    if (!tf) {
        close(fd);
        unlink(tmpname);
        return xf_val_nav(XF_TYPE_STR);
    }

    fwrite(sv.data.str->data, 1, sv.data.str->len, tf);
    fclose(tf); /* important: flush + close so child sees EOF */

    char shellcmd[8192];
    snprintf(shellcmd, sizeof(shellcmd), "%s < '%s'", cmd.data.str->data, tmpname);

    FILE *fp = popen(shellcmd, "r");
    if (!fp) {
        unlink(tmpname);
        return xf_val_nav(XF_TYPE_STR);
    }

    char buf[65536];
    size_t n = 0;
    int c;
    while (n < sizeof(buf) - 1 && (c = fgetc(fp)) != EOF)
        buf[n++] = (char)c;
    buf[n] = '\0';

    pclose(fp);
    unlink(tmpname);

    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';

    xf_Str *r = xf_str_from_cstr(buf);
    xf_Value rv = xf_val_ok_str(r);
    xf_str_release(r);
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
    /* LHS is always a string. RHS may be a compiled regex value or a str. */
    xf_Value sa = xf_coerce_str(a);
    if (sa.state != XF_STATE_OK || !sa.data.str)
        return make_bool(e->as.binary.op == BINOP_NMATCH);

    const char *subject = sa.data.str->data;
    const char *pattern = NULL;
    int cflags = REG_EXTENDED;

    if (b.state == XF_STATE_OK && b.type == XF_TYPE_REGEX && b.data.re) {
        /* regex literal value — use compiled pattern + flags */
        pattern = b.data.re->pattern->data;
        if (b.data.re->flags & XF_RE_ICASE)     cflags |= REG_ICASE;
        if (b.data.re->flags & XF_RE_MULTILINE)  cflags |= REG_NEWLINE;
    } else {
        xf_Value sb = xf_coerce_str(b);
        if (sb.state != XF_STATE_OK || !sb.data.str)
            return make_bool(e->as.binary.op == BINOP_NMATCH);
        pattern = sb.data.str->data;
    }

    regex_t re;
    if (regcomp(&re, pattern, cflags) != 0)
        return make_bool(e->as.binary.op == BINOP_NMATCH);
    int rc = regexec(&re, subject, 0, NULL, 0);
    regfree(&re);
    bool matched = (rc == 0);
    return make_bool(e->as.binary.op == BINOP_NMATCH ? !matched : matched);
}
                      
            default: return xf_val_nav(XF_TYPE_VOID);
        }
    }
    case EXPR_TERNARY: {
        xf_Value cond = interp_eval_expr(it, e->as.ternary.cond);
        return is_truthy(cond)
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
        if (s) { s->value = rhs; s->state = rhs.state; s->is_defined = true; }
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

                    Scope *popped = sym_pop(it->syms);
                    scope_free(popped);

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

            Scope *popped = sym_pop(it->syms);
            scope_free(popped);

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
            return xf_val_nav(XF_TYPE_NUM);
        }
        interp_error(it, e->loc, "unknown member '.%s'", field);
        return xf_val_nav(XF_TYPE_VOID);
    }
    case EXPR_PIPE_FN: {
        xf_Value left = interp_eval_expr(it, e->as.pipe_fn.left);
        if (e->as.pipe_fn.right->kind == EXPR_IDENT) {
            const char *name = e->as.pipe_fn.right->as.ident.name->data;
            return interp_call_builtin(it, name, &left, 1);
        }
        return left;
    }
    case EXPR_FN: {
        xf_Fn *fn = build_fn(NULL, e->as.fn.return_type,
                              e->as.fn.params, e->as.fn.param_count,
                              e->as.fn.body);
        e->as.fn.body = NULL;
        return xf_val_ok_fn(fn);
    }
    default:
        interp_error(it, e->loc, "unhandled expression kind %d", e->kind);
        return xf_val_nav(XF_TYPE_VOID);
    }
}
xf_Value interp_eval_stmt(Interp *it, Stmt *s) {
    if (!s) return xf_val_null();
    if (it->had_error || it->returning || it->exiting || it->nexting)
        return xf_val_null();
    switch (s->kind) {
    case STMT_BLOCK: {
        xf_Value last = xf_val_null();
        sym_push(it->syms, SCOPE_BLOCK);
        for (size_t i = 0; i < s->as.block.count; i++) {
            last = interp_eval_stmt(it, s->as.block.stmts[i]);
            if (it->returning || it->exiting || it->nexting || it->had_error) break;
        }
        sym_pop(it->syms);
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
            if (s->as.var_decl.type == XF_TYPE_NUM)
                init = xf_coerce_num(init);
            else if (s->as.var_decl.type == XF_TYPE_STR)
                init = xf_coerce_str(init);
        }
        Symbol *sym = sym_lookup_local(it->syms,
                          s->as.var_decl.name->data, s->as.var_decl.name->len);
        if (!sym)
            sym = sym_declare(it->syms, s->as.var_decl.name,
                              SYM_VAR, s->as.var_decl.type, s->loc);
        if (sym) { sym->value = init; sym->state = init.state; sym->is_defined = true; }
        return init;
    }
    case STMT_FN_DECL: {
        xf_Fn *fn = build_fn(s->as.fn_decl.name, s->as.fn_decl.return_type,
                              s->as.fn_decl.params, s->as.fn_decl.param_count,
                              s->as.fn_decl.body);
        s->as.fn_decl.body = NULL;
        xf_Value fv = xf_val_ok_fn(fn);
        Symbol *sym = sym_lookup_local(it->syms,
                          s->as.fn_decl.name->data, s->as.fn_decl.name->len);
        if (!sym)
            sym = sym_declare(it->syms, s->as.fn_decl.name, SYM_FN, XF_TYPE_FN, s->loc);
        if (sym) { sym->value = fv; sym->state = XF_STATE_OK; sym->is_defined = true; }
        return xf_val_null();
    }
    case STMT_IF: {
        for (size_t i = 0; i < s->as.if_stmt.count; i++) {
            xf_Value cond = interp_eval_expr(it, s->as.if_stmt.branches[i].cond);
            if (is_truthy(cond)) {
                sym_push(it->syms, SCOPE_BLOCK);
                xf_Value r = interp_eval_stmt(it, s->as.if_stmt.branches[i].body);
                sym_pop(it->syms);
                return r;
            }
        }
        if (s->as.if_stmt.els) {
            sym_push(it->syms, SCOPE_BLOCK);
            xf_Value r = interp_eval_stmt(it, s->as.if_stmt.els);
            sym_pop(it->syms);
            return r;
        }
        return xf_val_null();
    }
    case STMT_WHILE: {
        sym_push(it->syms, SCOPE_LOOP);
        while (is_truthy(interp_eval_expr(it, s->as.while_stmt.cond))) {
            interp_eval_stmt(it, s->as.while_stmt.body);
            if (it->nexting) { it->nexting = false; continue; }
            if (it->returning || it->exiting || it->had_error) break;
        }
        sym_pop(it->syms);
        return xf_val_null();
    }
    case STMT_WHILE_SHORT: {
        sym_push(it->syms, SCOPE_LOOP);
        while (is_truthy(interp_eval_expr(it, s->as.while_short.cond))) {
            interp_eval_stmt(it, s->as.while_short.body);
            if (it->nexting) { it->nexting = false; continue; }
            if (it->returning || it->exiting || it->had_error) break;
        }
        sym_pop(it->syms);
        return xf_val_null();
    }
    case STMT_FOR: {
        xf_Value col = interp_eval_expr(it, s->as.for_stmt.collection);
        if (col.state != XF_STATE_OK) return col;
        xf_Str *iter_name = s->as.for_stmt.iter;
        Stmt   *body      = s->as.for_stmt.body;

        if (col.type == XF_TYPE_ARR && col.data.arr) {
            xf_arr_t *a = col.data.arr;
            for (size_t i = 0; i < a->len; i++) {
                sym_push(it->syms, SCOPE_LOOP);
                Symbol *iv = sym_declare(it->syms, iter_name, SYM_VAR, XF_TYPE_VOID, s->loc);
                if (iv) { iv->value = a->items[i]; iv->state = a->items[i].state; iv->is_defined = true; }
                interp_eval_stmt(it, body);
                sym_pop(it->syms);
                if (it->nexting) { it->nexting = false; continue; }
                if (it->returning || it->exiting || it->had_error) break;
            }
        } else if ((col.type == XF_TYPE_MAP || col.type == XF_TYPE_SET) && col.data.map) {
            xf_map_t *m = col.data.map;
            for (size_t i = 0; i < m->order_len; i++) {
                xf_Str *key = m->order[i];
                xf_Value kv = xf_val_ok_str(key);
                sym_push(it->syms, SCOPE_LOOP);
                Symbol *iv = sym_declare(it->syms, iter_name, SYM_VAR, XF_TYPE_STR, s->loc);
                if (iv) { iv->value = kv; iv->state = XF_STATE_OK; iv->is_defined = true; }
                interp_eval_stmt(it, body);
                sym_pop(it->syms);
                if (it->nexting) { it->nexting = false; continue; }
                if (it->returning || it->exiting || it->had_error) break;
            }
        }
        return xf_val_null();
    }
    case STMT_FOR_SHORT: {
        xf_Value col = interp_eval_expr(it, s->as.for_short.collection);
        if (col.state != XF_STATE_OK) return col;
        xf_Str *iter_name = s->as.for_short.iter;
        Stmt   *body      = s->as.for_short.body;

        if (col.type == XF_TYPE_ARR && col.data.arr) {
            xf_arr_t *a = col.data.arr;
            for (size_t i = 0; i < a->len; i++) {
                sym_push(it->syms, SCOPE_LOOP);
                Symbol *iv = sym_declare(it->syms, iter_name, SYM_VAR, XF_TYPE_VOID, s->loc);
                if (iv) { iv->value = a->items[i]; iv->state = a->items[i].state; iv->is_defined = true; }
                interp_eval_stmt(it, body);
                sym_pop(it->syms);
                if (it->nexting) { it->nexting = false; continue; }
                if (it->returning || it->exiting || it->had_error) break;
            }
        } else if ((col.type == XF_TYPE_MAP || col.type == XF_TYPE_SET) && col.data.map) {
            xf_map_t *m = col.data.map;
            for (size_t i = 0; i < m->order_len; i++) {
                xf_Str *key = m->order[i];
                xf_Value kv = xf_val_ok_str(key);
                sym_push(it->syms, SCOPE_LOOP);
                Symbol *iv = sym_declare(it->syms, iter_name, SYM_VAR, XF_TYPE_STR, s->loc);
                if (iv) { iv->value = kv; iv->state = XF_STATE_OK; iv->is_defined = true; }
                interp_eval_stmt(it, body);
                sym_pop(it->syms);
                if (it->nexting) { it->nexting = false; continue; }
                if (it->returning || it->exiting || it->had_error) break;
            }
        }
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
        case STMT_PRINT: {
    xf_Value vals[64];
    size_t count = s->as.print.count < 64 ? s->as.print.count : 64;

    for (size_t i = 0; i < count; i++)
        vals[i] = interp_eval_expr(it, s->as.print.args[i]);

    uint8_t mode = it->vm->rec.out_mode;
    if (mode != XF_OUTFMT_TEXT) {
        print_structured(it, vals, count, mode);
    } else {
        for (size_t i = 0; i < count; i++) {
            if (i > 0) fputs(it->vm->rec.ofs, stdout);
            xf_print_value(stdout, vals[i]);
        }
        fputs(it->vm->rec.ors, stdout);
    }
    return xf_val_null();
}
        case STMT_PRINTF: {
        if (s->as.printf_stmt.count == 0) return xf_val_null();
        xf_Value fmtv = interp_eval_expr(it, s->as.printf_stmt.args[0]);
        xf_Value fmts = xf_coerce_str(fmtv);
        if (fmts.state != XF_STATE_OK || !fmts.data.str) return xf_val_null();
        xf_Value args[64];
        size_t argc = s->as.printf_stmt.count < 64 ? s->as.printf_stmt.count : 64;
        args[0] = fmtv;
        for (size_t i = 1; i < argc; i++)
            args[i] = interp_eval_expr(it, s->as.printf_stmt.args[i]);
        char buf[8192];
        xf_sprintf_impl(buf, sizeof(buf), fmts.data.str->data, args, argc);
        fputs(buf, stdout);
        return xf_val_null();
    }
    case STMT_OUTFMT:
        it->vm->rec.out_mode = s->as.outfmt.mode;
        return xf_val_null();
    case STMT_IMPORT:
        return xf_val_null();
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
            }
        }
        return xf_val_null();
    }
    case STMT_SPAWN:
        interp_eval_expr(it, s->as.spawn.call);
        return xf_val_null();
    case STMT_JOIN:
        return xf_val_null();
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
            top->as.fn.body = NULL;
            xf_Value fv = xf_val_ok_fn(fn);
            Symbol *sym = sym_lookup_local(it->syms,
                              top->as.fn.name->data, top->as.fn.name->len);
            if (!sym)
                sym = sym_declare(it->syms, top->as.fn.name, SYM_FN, XF_TYPE_FN, top->loc);
            if (sym) { sym->value = fv; sym->state = XF_STATE_OK; sym->is_defined = true; }
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
}
void interp_feed_record(Interp *it, Program *prog,
                        const char *rec, size_t len) {
    vm_split_record(it->vm, rec, len);
    for (size_t i = 0; i < prog->count; i++) {
        TopLevel *t = prog->items[i];
        if (t->kind != TOP_RULE) continue;
        if (t->as.rule.pattern) {
            xf_Value pat = interp_eval_expr(it, t->as.rule.pattern);
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