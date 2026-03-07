#include "../include/core.h"
#include "../include/value.h"
#include "../include/symTable.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

/* ============================================================
 * Helpers
 * ============================================================ */

/* require at least n args; return NAV if fewer given */
#define NEED(n) \
    do { if (argc < (n)) return xf_val_nav(XF_TYPE_VOID); } while(0)
static xf_module_t *build_ds(void);
/* require arg i to be OK num, else propagate / return NAV */
static bool arg_num(xf_Value *args, size_t argc, size_t i, double *out) {
    if (i >= argc) return false;
    xf_Value v = args[i];
    if (v.state != XF_STATE_OK) return false;
    if (v.type == XF_TYPE_NUM)  { *out = v.data.num; return true; }
    /* try str → num coerce */
    xf_Value c = xf_coerce_num(v);
    if (c.state == XF_STATE_OK) { *out = c.data.num; return true; }
    return false;
}

static bool arg_str(xf_Value *args, size_t argc, size_t i,
                    const char **out, size_t *outlen) {
    if (i >= argc) return false;
    xf_Value v = args[i];
    if (v.state != XF_STATE_OK) return false;
    xf_Value c = xf_coerce_str(v);
    if (c.state != XF_STATE_OK) return false;
    if (c.data.str) { *out = c.data.str->data; *outlen = c.data.str->len; }
    else            { *out = "";               *outlen = 0; }
    return true;
}

static xf_Value make_str_val(const char *data, size_t len);

/* propagate first non-OK arg state */
static xf_Value propagate(xf_Value *args, size_t argc) {
    for (size_t i = 0; i < argc; i++)
        if (args[i].state != XF_STATE_OK) return args[i];
    return xf_val_nav(XF_TYPE_VOID);
}

#define MATH1(c_fn) \
    do { \
        double x; \
        if (!arg_num(args, argc, 0, &x)) return propagate(args, argc); \
        return xf_val_ok_num(c_fn(x)); \
    } while(0)

#define MATH2(c_fn) \
    do { \
        double x, y; \
        if (!arg_num(args, argc, 0, &x)) return propagate(args, argc); \
        if (!arg_num(args, argc, 1, &y)) return propagate(args, argc); \
        return xf_val_ok_num(c_fn(x, y)); \
    } while(0)


/* ============================================================
 * core.math native functions
 * ============================================================ */

static xf_Value cm_sin(xf_Value *args, size_t argc)   { NEED(1); MATH1(sin);   }
static xf_Value cm_cos(xf_Value *args, size_t argc)   { NEED(1); MATH1(cos);   }
static xf_Value cm_tan(xf_Value *args, size_t argc)   { NEED(1); MATH1(tan);   }
static xf_Value cm_asin(xf_Value *args, size_t argc)  { NEED(1); MATH1(asin);  }
static xf_Value cm_acos(xf_Value *args, size_t argc)  { NEED(1); MATH1(acos);  }
static xf_Value cm_atan(xf_Value *args, size_t argc)  { NEED(1); MATH1(atan);  }
static xf_Value cm_sqrt(xf_Value *args, size_t argc)  { NEED(1); MATH1(sqrt);  }
static xf_Value cm_exp(xf_Value *args, size_t argc)   { NEED(1); MATH1(exp);   }
static xf_Value cm_log(xf_Value *args, size_t argc)   { NEED(1); MATH1(log);   }
static xf_Value cm_log2(xf_Value *args, size_t argc)  { NEED(1); MATH1(log2);  }
static xf_Value cm_log10(xf_Value *args, size_t argc) { NEED(1); MATH1(log10); }
static xf_Value cm_abs(xf_Value *args, size_t argc)   { NEED(1); MATH1(fabs);  }
static xf_Value cm_floor(xf_Value *args, size_t argc) { NEED(1); MATH1(floor); }
static xf_Value cm_ceil(xf_Value *args, size_t argc)  { NEED(1); MATH1(ceil);  }
static xf_Value cm_round(xf_Value *args, size_t argc) { NEED(1); MATH1(round); }
static xf_Value cm_int(xf_Value *args, size_t argc)   { NEED(1); MATH1(trunc); }

static xf_Value cm_atan2(xf_Value *args, size_t argc) { NEED(2); MATH2(atan2); }
static xf_Value cm_pow(xf_Value *args, size_t argc)   { NEED(2); MATH2(pow);   }

static xf_Value cm_min(xf_Value *args, size_t argc) {
    NEED(2);
    double x, y;
    if (!arg_num(args, argc, 0, &x)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &y)) return propagate(args, argc);
    return xf_val_ok_num(x < y ? x : y);
}

static xf_Value cm_max(xf_Value *args, size_t argc) {
    NEED(2);
    double x, y;
    if (!arg_num(args, argc, 0, &x)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &y)) return propagate(args, argc);
    return xf_val_ok_num(x > y ? x : y);
}

static xf_Value cm_clamp(xf_Value *args, size_t argc) {
    NEED(3);
    double v, lo, hi;
    if (!arg_num(args, argc, 0, &v))  return propagate(args, argc);
    if (!arg_num(args, argc, 1, &lo)) return propagate(args, argc);
    if (!arg_num(args, argc, 2, &hi)) return propagate(args, argc);
    return xf_val_ok_num(v < lo ? lo : v > hi ? hi : v);
}

static xf_Value cm_rand(xf_Value *args, size_t argc) {
    
    argc=argc-1;
    if(args) argc=argc+1;
    return xf_val_ok_num((double)rand() / (double)RAND_MAX);
}

static xf_Value cm_srand(xf_Value *args, size_t argc) {
    double seed;
    if (arg_num(args, argc, 0, &seed)) srand((unsigned)seed);
    else                          srand((unsigned)time(NULL));
    return xf_val_null();
}


/* ============================================================
 * core.str native functions
 * ============================================================ */

static xf_Value cs_len(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    return xf_val_ok_num((double)slen);
}

static xf_Value cs_upper(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    char *buf = malloc(slen + 1);
    for (size_t i = 0; i < slen; i++) buf[i] = (char)toupper((unsigned char)s[i]);
    buf[slen] = '\0';
    xf_Str *r = xf_str_new(buf, slen);
    free(buf);
    xf_Value v = xf_val_ok_str(r);
    xf_str_release(r);
    return v;
}

static xf_Value cs_lower(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    char *buf = malloc(slen + 1);
    for (size_t i = 0; i < slen; i++) buf[i] = (char)tolower((unsigned char)s[i]);
    buf[slen] = '\0';
    xf_Str *r = xf_str_new(buf, slen);
    free(buf);
    xf_Value v = xf_val_ok_str(r);
    xf_str_release(r);
    return v;
}

/* trim helpers */
static xf_Value make_str_val(const char *data, size_t len) {
    xf_Str *s = xf_str_new(data, len);
    xf_Value v = xf_val_ok_str(s);
    xf_str_release(s);
    return v;
}
static xf_Value cs_capitalize(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    if (slen == 0) return make_str_val(s, 0);
    char *buf = malloc(slen + 1);
    buf[0] = (char)toupper((unsigned char)s[0]);
    for (size_t i = 1; i < slen; i++) buf[i] = (char)tolower((unsigned char)s[i]);
    buf[slen] = '\0';
    xf_Str *r = xf_str_new(buf, slen);
    free(buf);
    xf_Value v = xf_val_ok_str(r);
    xf_str_release(r);
    return v;
}

static xf_Value cs_trim(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    size_t lo = 0, hi = slen;
    while (lo < hi && isspace((unsigned char)s[lo])) lo++;
    while (hi > lo && isspace((unsigned char)s[hi-1])) hi--;
    return make_str_val(s + lo, hi - lo);
}

static xf_Value cs_ltrim(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    size_t lo = 0;
    while (lo < slen && isspace((unsigned char)s[lo])) lo++;
    return make_str_val(s + lo, slen - lo);
}

static xf_Value cs_rtrim(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    size_t hi = slen;
    while (hi > 0 && isspace((unsigned char)s[hi-1])) hi--;
    return make_str_val(s, hi);
}

static xf_Value cs_substr(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    double dstart;
    if (!arg_num(args, argc, 1, &dstart)) return propagate(args, argc);
    size_t start = (size_t)(dstart < 0 ? 0 : dstart);
    if (start >= slen) return make_str_val("", 0);
    size_t take = slen - start;
    if (argc >= 3) {
        double dlen;
        if (arg_num(args, argc, 2, &dlen) && dlen >= 0 && (size_t)dlen < take)
            take = (size_t)dlen;
    }
    return make_str_val(s + start, take);
}

static xf_Value cs_index(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen;
    const char *needle; size_t nlen;
    if (!arg_str(args, argc, 0, &s,      &slen)) return propagate(args, argc);
    if (!arg_str(args, argc, 1, &needle, &nlen)) return propagate(args, argc);
    if (nlen == 0) return xf_val_ok_num(0);
    const char *found = strstr(s, needle);
    return xf_val_ok_num(found ? (double)(found - s) : -1.0);
}

static xf_Value cs_contains(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen;
    const char *needle; size_t nlen;
    if (!arg_str(args, argc, 0, &s,      &slen)) return propagate(args, argc);
    if (!arg_str(args, argc, 1, &needle, &nlen)) return propagate(args, argc);
    return xf_val_ok_num(strstr(s, needle) ? 1.0 : 0.0);
}

static xf_Value cs_starts_with(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen;
    const char *pre; size_t prelen;
    if (!arg_str(args, argc, 0, &s,   &slen))   return propagate(args, argc);
    if (!arg_str(args, argc, 1, &pre, &prelen)) return propagate(args, argc);
    if (prelen > slen) return xf_val_ok_num(0);
    return xf_val_ok_num(memcmp(s, pre, prelen) == 0 ? 1.0 : 0.0);
}

static xf_Value cs_ends_with(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen;
    const char *suf; size_t suflen;
    if (!arg_str(args, argc, 0, &s,   &slen))   return propagate(args, argc);
    if (!arg_str(args, argc, 1, &suf, &suflen)) return propagate(args, argc);
    if (suflen > slen) return xf_val_ok_num(0);
    return xf_val_ok_num(memcmp(s + slen - suflen, suf, suflen) == 0 ? 1.0 : 0.0);
}

static xf_Value cs_replace(xf_Value *args, size_t argc) {
    NEED(3);
    const char *s; size_t slen;
    const char *old; size_t oldlen;
    const char *neo; size_t neolen;
    if (!arg_str(args, argc, 0, &s,   &slen))   return propagate(args, argc);
    if (!arg_str(args, argc, 1, &old, &oldlen)) return propagate(args, argc);
    if (!arg_str(args, argc, 2, &neo, &neolen)) return propagate(args, argc);
    if (oldlen == 0) return make_str_val(s, slen);
    const char *found = strstr(s, old);
    if (!found) return make_str_val(s, slen);
    size_t prefix = (size_t)(found - s);
    size_t total  = prefix + neolen + (slen - prefix - oldlen);
    char *buf = malloc(total + 1);
    memcpy(buf, s, prefix);
    memcpy(buf + prefix, neo, neolen);
    memcpy(buf + prefix + neolen, found + oldlen, slen - prefix - oldlen);
    buf[total] = '\0';
    xf_Value v = make_str_val(buf, total);
    free(buf);
    return v;
}

static xf_Value cs_replace_all(xf_Value *args, size_t argc) {
    NEED(3);
    const char *s; size_t slen;
    const char *old; size_t oldlen;
    const char *neo; size_t neolen;
    if (!arg_str(args, argc, 0, &s,   &slen))   return propagate(args, argc);
    if (!arg_str(args, argc, 1, &old, &oldlen)) return propagate(args, argc);
    if (!arg_str(args, argc, 2, &neo, &neolen)) return propagate(args, argc);
    if (oldlen == 0) return make_str_val(s, slen);

    /* build result with a dynamic buffer */
    size_t cap = slen * 2 + 64;
    char *buf = malloc(cap);
    size_t wpos = 0;
    const char *cur = s;
    const char *end = s + slen;

    while (cur < end) {
        const char *found = strstr(cur, old);
        if (!found) {
            size_t rest = (size_t)(end - cur);
            if (wpos + rest + 1 > cap) { cap = wpos + rest + 64; buf = realloc(buf, cap); }
            memcpy(buf + wpos, cur, rest);
            wpos += rest;
            break;
        }
        size_t prefix = (size_t)(found - cur);
        if (wpos + prefix + neolen + 1 > cap) { cap = (wpos + prefix + neolen) * 2 + 64; buf = realloc(buf, cap); }
        memcpy(buf + wpos, cur, prefix);
        wpos += prefix;
        memcpy(buf + wpos, neo, neolen);
        wpos += neolen;
        cur = found + oldlen;
    }
    buf[wpos] = '\0';
    xf_Value v = make_str_val(buf, wpos);
    free(buf);
    return v;
}

static xf_Value cs_repeat(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen;
    double dn;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &dn))       return propagate(args, argc);
    size_t times = dn > 0 ? (size_t)dn : 0;
    size_t total = slen * times;
    char *buf = malloc(total + 1);
    for (size_t i = 0; i < times; i++) memcpy(buf + i * slen, s, slen);
    buf[total] = '\0';
    xf_Value v = make_str_val(buf, total);
    free(buf);
    return v;
}

static xf_Value cs_reverse(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    char *buf = malloc(slen + 1);
    for (size_t i = 0; i < slen; i++) buf[i] = s[slen - 1 - i];
    buf[slen] = '\0';
    xf_Value v = make_str_val(buf, slen);
    free(buf);
    return v;
}

static xf_Value cs_sprintf(xf_Value *args, size_t argc) {
    NEED(1);
    const char *fmt; size_t fmtlen;
    if (!arg_str(args, argc, 0, &fmt, &fmtlen)) return propagate(args, argc);
    /* single-arg simple sprintf via snprintf */
    char buf[4096];
    if (argc >= 2) {
        xf_Value v2 = xf_coerce_str(args[1]);
        if (v2.state == XF_STATE_OK && v2.data.str)
            snprintf(buf, sizeof(buf), fmt, v2.data.str->data);
        else
            snprintf(buf, sizeof(buf), "%s", fmt);
    } else {
        snprintf(buf, sizeof(buf), "%s", fmt);
    }
    return make_str_val(buf, strlen(buf));
}


/* ============================================================
 * core.os native functions
 * ============================================================ */

static xf_Value csy_exec(xf_Value *args, size_t argc) {
    NEED(1);
    const char *cmd; size_t cmdlen;
    if (!arg_str(args, argc, 0, &cmd, &cmdlen)) return propagate(args, argc);
    int rc = system(cmd);
    return xf_val_ok_num((double)rc);
}

static xf_Value csy_exit(xf_Value *args, size_t argc) {
    double code = 0;
    arg_num(args, argc, 0, &code);
    exit((int)code);
}

static xf_Value csy_time(xf_Value *args, size_t argc) {
    argc=argc-1;
    if(args) argc=argc+1;
    return xf_val_ok_num((double)time(NULL));
}

static xf_Value csy_env(xf_Value *args, size_t argc) {
    NEED(1);
    const char *name; size_t namelen;
    if (!arg_str(args, argc, 0, &name, &namelen)) return propagate(args, argc);
    const char *val = getenv(name);
    if (!val) return xf_val_nav(XF_TYPE_STR);
    return make_str_val(val, strlen(val));
}


/* ============================================================
 * Module construction + registration
 * ============================================================ */

#define FN(name, ret, impl) \
    xf_module_set(m, name, xf_val_native_fn(name, ret, impl))

/* ============================================================
 * core.generics native functions
 * ============================================================ */

/* join(coll, sep)
 *   arr      -> elements stringified, joined with sep
 *   map      -> insertion-ordered values joined with sep
 *   set      -> insertion-ordered keys joined with sep
 *   str      -> join(a, sep, b) joins two strings with sep
 *   num      -> join(a, sep, b) joins two nums as str; re-coerces to num
 *              if the result is not numeric, warns and returns str        */
static xf_Value cg_join(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value coll = args[0];
    if (coll.state != XF_STATE_OK) return coll;

    const char *sep; size_t seplen;
    if (!arg_str(args, argc, 1, &sep, &seplen)) return propagate(args, argc);

    /* num: needs a third argument (the other operand) */
    if (coll.type == XF_TYPE_NUM) {
        if (argc < 3) return xf_val_nav(XF_TYPE_VOID);
        xf_Value as = xf_coerce_str(coll);
        xf_Value bs = xf_coerce_str(args[2]);
        if (as.state != XF_STATE_OK || bs.state != XF_STATE_OK)
            return propagate(args, argc);
        size_t alen = as.data.str->len, blen = bs.data.str->len;
        size_t total = alen + seplen + blen;
        char *buf = malloc(total + 1);
        memcpy(buf,                    as.data.str->data, alen);
        memcpy(buf + alen,             sep,               seplen);
        memcpy(buf + alen + seplen,    bs.data.str->data, blen);
        buf[total] = '\0';
        xf_Value joined = make_str_val(buf, total);
        free(buf);
        xf_str_release(as.data.str);
        xf_str_release(bs.data.str);
        xf_Value num_try = xf_coerce_num(joined);
        if (num_try.state == XF_STATE_OK) return num_try;
        fprintf(stderr, "xf warning: join of num values produced non-numeric "
                        "result, returning str\n");
        return joined;
    }

    /* str: join(a, sep, b) */
    if (coll.type == XF_TYPE_STR && coll.data.str) {
        if (argc < 3) return coll;
        const char *a = coll.data.str->data;
        size_t alen   = coll.data.str->len;
        const char *b; size_t blen;
        if (!arg_str(args, argc, 2, &b, &blen)) return propagate(args, argc);
        size_t total = alen + seplen + blen;
        char *buf = malloc(total + 1);
        memcpy(buf,                a,   alen);
        memcpy(buf + alen,         sep, seplen);
        memcpy(buf + alen + seplen, b,  blen);
        buf[total] = '\0';
        xf_Value v = make_str_val(buf, total);
        free(buf);
        return v;
    }

    /* arr */
    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        xf_arr_t *a = coll.data.arr;
        size_t total = 0;
        for (size_t i = 0; i < a->len; i++) {
            xf_Value sv = xf_coerce_str(a->items[i]);
            if (sv.state == XF_STATE_OK && sv.data.str)
                total += sv.data.str->len;
            if (i + 1 < a->len) total += seplen;
        }
        char *buf = malloc(total + 1);
        size_t pos = 0;
        for (size_t i = 0; i < a->len; i++) {
            xf_Value sv = xf_coerce_str(a->items[i]);
            if (sv.state == XF_STATE_OK && sv.data.str) {
                memcpy(buf + pos, sv.data.str->data, sv.data.str->len);
                pos += sv.data.str->len;
            }
            if (i + 1 < a->len) { memcpy(buf + pos, sep, seplen); pos += seplen; }
        }
        buf[pos] = '\0';
        xf_Value v = make_str_val(buf, pos);
        free(buf);
        return v;
    }

    /* map (values) or set (keys) */
    if ((coll.type == XF_TYPE_MAP || coll.type == XF_TYPE_SET) && coll.data.map) {
        xf_map_t *m = coll.data.map;
        bool is_set = (coll.type == XF_TYPE_SET);
        size_t total = 0;
        for (size_t i = 0; i < m->order_len; i++) {
            xf_Value sv;
            if (is_set) sv = xf_val_ok_str(m->order[i]);
            else { xf_Value val = xf_map_get(m, m->order[i]); sv = xf_coerce_str(val); }
            if (sv.state == XF_STATE_OK && sv.data.str)
                total += sv.data.str->len;
            if (i + 1 < m->order_len) total += seplen;
        }
        char *buf = malloc(total + 1);
        size_t pos = 0;
        for (size_t i = 0; i < m->order_len; i++) {
            xf_Value sv;
            if (is_set) sv = xf_val_ok_str(m->order[i]);
            else { xf_Value val = xf_map_get(m, m->order[i]); sv = xf_coerce_str(val); }
            if (sv.state == XF_STATE_OK && sv.data.str) {
                memcpy(buf + pos, sv.data.str->data, sv.data.str->len);
                pos += sv.data.str->len;
            }
            if (i + 1 < m->order_len) { memcpy(buf + pos, sep, seplen); pos += seplen; }
        }
        buf[pos] = '\0';
        xf_Value v = make_str_val(buf, pos);
        free(buf);
        return v;
    }

    return xf_val_nav(XF_TYPE_STR);
}

/* split(str, sep) -> arr of substrings; sep="" splits into chars */
static xf_Value cg_split(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value src = args[0];
    if (src.state != XF_STATE_OK) return src;
    if (src.type == XF_TYPE_ARR || src.type == XF_TYPE_MAP ||
        src.type == XF_TYPE_SET)
        return xf_val_nav(XF_TYPE_ARR);   /* future: partition overload */

    const char *s; size_t slen;
    const char *sep; size_t seplen;
    if (!arg_str(args, argc, 0, &s,   &slen))   return propagate(args, argc);
    if (!arg_str(args, argc, 1, &sep, &seplen)) return propagate(args, argc);

    xf_arr_t *out = xf_arr_new();
    if (seplen == 0) {
        for (size_t i = 0; i < slen; i++) xf_arr_push(out, make_str_val(s + i, 1));
    } else {
        const char *p = s, *end = s + slen;
        while (p <= end) {
            const char *found = (p < end) ? strstr(p, sep) : NULL;
            const char *seg_end = found ? found : end;
            xf_arr_push(out, make_str_val(p, (size_t)(seg_end - p)));
            if (!found) break;
            p = found + seplen;
        }
    }
    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}

/* strip(str [,chars])  -> trim both ends
 * strip(arr [,chars])  -> new arr, each str element trimmed
 * strip(map [,chars])  -> new map, each str value trimmed
 * strip(set [,chars])  -> new set, each member key trimmed           */
static xf_Value cg_strip(xf_Value *args, size_t argc) {
    NEED(1);
    xf_Value v = args[0];
    if (v.state != XF_STATE_OK) return v;

    const char *chars = NULL; size_t chars_len = 0;
    bool has_chars = (argc >= 2 && args[1].state == XF_STATE_OK &&
                      args[1].type == XF_TYPE_STR);
    if (has_chars) arg_str(args, argc, 1, &chars, &chars_len);

#define STRIP_CHAR(c) \
    (has_chars ? (chars_len > 0 && \
                  memchr(chars, (unsigned char)(c), chars_len) != NULL) \
               : isspace((unsigned char)(c)))

    if (v.type == XF_TYPE_STR && v.data.str) {
        const char *s = v.data.str->data;
        size_t lo = 0, hi = v.data.str->len;
        while (lo < hi && STRIP_CHAR(s[lo]))    lo++;
        while (hi > lo && STRIP_CHAR(s[hi-1])) hi--;
        return make_str_val(s + lo, hi - lo);
    }

    if (v.type == XF_TYPE_ARR && v.data.arr) {
        xf_arr_t *in = v.data.arr, *out = xf_arr_new();
        for (size_t i = 0; i < in->len; i++) {
            xf_Value e = in->items[i];
            if (e.state == XF_STATE_OK && e.type == XF_TYPE_STR && e.data.str) {
                const char *s = e.data.str->data;
                size_t lo = 0, hi = e.data.str->len;
                while (lo < hi && STRIP_CHAR(s[lo]))    lo++;
                while (hi > lo && STRIP_CHAR(s[hi-1])) hi--;
                xf_arr_push(out, make_str_val(s + lo, hi - lo));
            } else {
                xf_arr_push(out, e);
            }
        }
        xf_Value rv = xf_val_ok_arr(out); xf_arr_release(out); return rv;
    }

    if (v.type == XF_TYPE_MAP && v.data.map) {
        xf_map_t *in = v.data.map, *out = xf_map_new();
        for (size_t i = 0; i < in->order_len; i++) {
            xf_Str *key = in->order[i];
            xf_Value val = xf_map_get(in, key);
            if (val.state == XF_STATE_OK && val.type == XF_TYPE_STR && val.data.str) {
                const char *s = val.data.str->data;
                size_t lo = 0, hi = val.data.str->len;
                while (lo < hi && STRIP_CHAR(s[lo]))    lo++;
                while (hi > lo && STRIP_CHAR(s[hi-1])) hi--;
                xf_map_set(out, key, make_str_val(s + lo, hi - lo));
            } else {
                xf_map_set(out, key, val);
            }
        }
        xf_Value rv = xf_val_ok_map(out); return rv;
    }

    if (v.type == XF_TYPE_SET && v.data.map) {
        xf_map_t *in = v.data.map, *out = xf_map_new();
        for (size_t i = 0; i < in->order_len; i++) {
            xf_Str *key = in->order[i];
            const char *s = key->data;
            size_t lo = 0, hi = key->len;
            while (lo < hi && STRIP_CHAR(s[lo]))    lo++;
            while (hi > lo && STRIP_CHAR(s[hi-1])) hi--;
            xf_Str *nk = xf_str_new(s + lo, hi - lo);
            xf_map_set(out, nk, xf_val_ok_num(1.0));
            xf_str_release(nk);
        }
        xf_Value rv = xf_val_ok_map(out);
        rv.type = XF_TYPE_SET;
        return rv;
    }

#undef STRIP_CHAR
    return xf_val_nav(XF_TYPE_VOID);
}

/* contains(str, needle) -> 1 if substring found
 * contains(arr, val)    -> 1 if any element matches (string comparison)
 * contains(map, key)    -> 1 if key exists
 * contains(set, member) -> 1 if member exists                         */
static xf_Value cg_contains(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value coll   = args[0];
    xf_Value needle = args[1];
    if (coll.state   != XF_STATE_OK) return coll;
    if (needle.state != XF_STATE_OK) return needle;

    if (coll.type == XF_TYPE_STR && coll.data.str) {
        xf_Value ns = xf_coerce_str(needle);
        if (ns.state != XF_STATE_OK || !ns.data.str) return xf_val_ok_num(0.0);
        return xf_val_ok_num(strstr(coll.data.str->data, ns.data.str->data) ? 1.0 : 0.0);
    }

    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        xf_Value ns = xf_coerce_str(needle);
        if (ns.state != XF_STATE_OK || !ns.data.str) return xf_val_ok_num(0.0);
        xf_arr_t *a = coll.data.arr;
        for (size_t i = 0; i < a->len; i++) {
            xf_Value es = xf_coerce_str(a->items[i]);
            if (es.state == XF_STATE_OK && es.data.str &&
                strcmp(es.data.str->data, ns.data.str->data) == 0)
                return xf_val_ok_num(1.0);
        }
        return xf_val_ok_num(0.0);
    }

    if ((coll.type == XF_TYPE_MAP || coll.type == XF_TYPE_SET) && coll.data.map) {
        xf_Value ks = xf_coerce_str(needle);
        if (ks.state != XF_STATE_OK || !ks.data.str) return xf_val_ok_num(0.0);
        xf_Value got = xf_map_get(coll.data.map, ks.data.str);
        return xf_val_ok_num(got.state == XF_STATE_OK ? 1.0 : 0.0);
    }

    return xf_val_ok_num(0.0);
}

/* length(str) -> byte count
 * length(arr) -> element count
 * length(map) -> key count
 * length(set) -> member count
 * length(num) -> sizeof(double) = 8                                   */
static xf_Value cg_length(xf_Value *args, size_t argc) {
    NEED(1);
    xf_Value v = args[0];
    if (v.state != XF_STATE_OK) return v;
    switch (v.type) {
        case XF_TYPE_STR: return xf_val_ok_num(v.data.str  ? (double)v.data.str->len        : 0.0);
        case XF_TYPE_ARR: return xf_val_ok_num(v.data.arr  ? (double)v.data.arr->len        : 0.0);
        case XF_TYPE_MAP:
        case XF_TYPE_SET: return xf_val_ok_num(v.data.map  ? (double)v.data.map->order_len  : 0.0);
        case XF_TYPE_NUM: return xf_val_ok_num((double)sizeof(double));
        default:          return xf_val_nav(XF_TYPE_NUM);
    }
}

static xf_module_t *build_generics(void) {
    xf_module_t *m = xf_module_new("core.generics");
    FN("join",     XF_TYPE_STR, cg_join);
    FN("split",    XF_TYPE_ARR, cg_split);
    FN("strip",    XF_TYPE_STR, cg_strip);
    FN("contains", XF_TYPE_NUM, cg_contains);
    FN("length",   XF_TYPE_NUM, cg_length);
    return m;
}


static xf_module_t *build_math(void) {
    xf_module_t *m = xf_module_new("core.math");

    FN("sin",   XF_TYPE_NUM, cm_sin);
    FN("cos",   XF_TYPE_NUM, cm_cos);
    FN("tan",   XF_TYPE_NUM, cm_tan);
    FN("asin",  XF_TYPE_NUM, cm_asin);
    FN("acos",  XF_TYPE_NUM, cm_acos);
    FN("atan",  XF_TYPE_NUM, cm_atan);
    FN("atan2", XF_TYPE_NUM, cm_atan2);
    FN("sqrt",  XF_TYPE_NUM, cm_sqrt);
    FN("pow",   XF_TYPE_NUM, cm_pow);
    FN("exp",   XF_TYPE_NUM, cm_exp);
    FN("log",   XF_TYPE_NUM, cm_log);
    FN("log2",  XF_TYPE_NUM, cm_log2);
    FN("log10", XF_TYPE_NUM, cm_log10);
    FN("abs",   XF_TYPE_NUM, cm_abs);
    FN("floor", XF_TYPE_NUM, cm_floor);
    FN("ceil",  XF_TYPE_NUM, cm_ceil);
    FN("round", XF_TYPE_NUM, cm_round);
    FN("int",   XF_TYPE_NUM, cm_int);
    FN("min",   XF_TYPE_NUM, cm_min);
    FN("max",   XF_TYPE_NUM, cm_max);
    FN("clamp", XF_TYPE_NUM, cm_clamp);
    FN("rand",  XF_TYPE_NUM, cm_rand);
    FN("srand", XF_TYPE_VOID, cm_srand);

    /* numeric constants */
    xf_module_set(m, "PI",  xf_val_ok_num(M_PI));
    xf_module_set(m, "E",   xf_val_ok_num(M_E));
    xf_module_set(m, "INF", xf_val_ok_num(INFINITY));
    xf_module_set(m, "NAN", xf_val_ok_num(NAN));

    return m;
}

static xf_Value cs_concat(xf_Value *args, size_t argc) {
    NEED(1);
    /* concat all args as strings into one */
    size_t total = 0;
    const char *parts[64];
    size_t lens[64];
    size_t n = argc < 64 ? argc : 64;
    for (size_t i = 0; i < n; i++) {
        if (!arg_str(args, argc, i, &parts[i], &lens[i])) {
            parts[i] = ""; lens[i] = 0;
        }
        total += lens[i];
    }
    char *buf = malloc(total + 1);
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        memcpy(buf + pos, parts[i], lens[i]);
        pos += lens[i];
    }
    buf[total] = '\0';
    xf_Value v = make_str_val(buf, total);
    free(buf);
    return v;
}

/* comp(a, b) → -1 / 0 / 1  (lexicographic, like strcmp) */
static xf_Value cs_comp(xf_Value *args, size_t argc) {
    NEED(2);
    const char *a; size_t alen;
    const char *b; size_t blen;
    if (!arg_str(args, argc, 0, &a, &alen)) return propagate(args, argc);
    if (!arg_str(args, argc, 1, &b, &blen)) return propagate(args, argc);
    int cmp = strcmp(a, b);
    return xf_val_ok_num(cmp < 0 ? -1.0 : cmp > 0 ? 1.0 : 0.0);
}

static xf_module_t *build_str(void) {
    xf_module_t *m = xf_module_new("core.str");

    FN("len",         XF_TYPE_NUM,  cs_len);
    FN("upper",       XF_TYPE_STR,  cs_upper);
    FN("lower",       XF_TYPE_STR,  cs_lower);
    FN("capitalize",  XF_TYPE_STR,  cs_capitalize);
    FN("trim",        XF_TYPE_STR,  cs_trim);
    FN("ltrim",       XF_TYPE_STR,  cs_ltrim);
    FN("rtrim",       XF_TYPE_STR,  cs_rtrim);
    FN("substr",      XF_TYPE_STR,  cs_substr);
    FN("index",       XF_TYPE_NUM,  cs_index);
    FN("contains",    XF_TYPE_NUM,  cs_contains);
    FN("starts_with", XF_TYPE_NUM,  cs_starts_with);
    FN("ends_with",   XF_TYPE_NUM,  cs_ends_with);
    FN("replace",     XF_TYPE_STR,  cs_replace);
    FN("replace_all", XF_TYPE_STR,  cs_replace_all);
    FN("repeat",      XF_TYPE_STR,  cs_repeat);
    FN("reverse",     XF_TYPE_STR,  cs_reverse);
    FN("sprintf",     XF_TYPE_STR,  cs_sprintf);
    FN("concat",      XF_TYPE_STR,  cs_concat);
    FN("comp",        XF_TYPE_NUM,  cs_comp);

    return m;
}

static xf_Value csy_read(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen;
    if (!arg_str(args, argc, 0, &path, &plen)) return propagate(args, argc);
    FILE *fp = fopen(path, "r");
    if (!fp) return xf_val_nav(XF_TYPE_STR);
    char buf[65536]; size_t n = 0; int c;
    while (n < sizeof(buf) - 1 && (c = fgetc(fp)) != EOF) buf[n++] = (char)c;
    buf[n] = '\0'; fclose(fp);
    return make_str_val(buf, n);
}

static xf_Value csy_write(xf_Value *args, size_t argc) {
    NEED(2);
    const char *path; size_t plen;
    const char *data; size_t dlen;
    if (!arg_str(args, argc, 0, &path, &plen)) return propagate(args, argc);
    if (!arg_str(args, argc, 1, &data, &dlen)) return propagate(args, argc);
    FILE *fp = fopen(path, "w");
    if (!fp) return xf_val_ok_num(0);
    fwrite(data, 1, dlen, fp); fclose(fp);
    return xf_val_ok_num(1);
}

static xf_Value csy_append(xf_Value *args, size_t argc) {
    NEED(2);
    const char *path; size_t plen;
    const char *data; size_t dlen;
    if (!arg_str(args, argc, 0, &path, &plen)) return propagate(args, argc);
    if (!arg_str(args, argc, 1, &data, &dlen)) return propagate(args, argc);
    FILE *fp = fopen(path, "a");
    if (!fp) return xf_val_ok_num(0);
    fwrite(data, 1, dlen, fp); fclose(fp);
    return xf_val_ok_num(1);
}

static xf_Value csy_lines(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen;
    if (!arg_str(args, argc, 0, &path, &plen)) return propagate(args, argc);
    FILE *fp = fopen(path, "r");
    if (!fp) return xf_val_nav(XF_TYPE_ARR);
    xf_arr_t *a = xf_arr_new();
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r')) line[--ln] = '\0';
        xf_Str *ls = xf_str_new(line, ln);
        xf_Value lv = xf_val_ok_str(ls); xf_str_release(ls);
        xf_arr_push(a, lv);
    }
    fclose(fp);
    xf_Value r = xf_val_ok_arr(a); xf_arr_release(a); return r;
}

static xf_Value csy_run(xf_Value *args, size_t argc) {
    NEED(1);
    const char *cmd; size_t cmdlen;
    if (!arg_str(args, argc, 0, &cmd, &cmdlen)) return propagate(args, argc);
    FILE *fp = popen(cmd, "r");
    if (!fp) return xf_val_nav(XF_TYPE_STR);
    char buf[65536]; size_t n = 0; int c;
    while (n < sizeof(buf) - 1 && (c = fgetc(fp)) != EOF) buf[n++] = (char)c;
    buf[n] = '\0'; pclose(fp);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    return make_str_val(buf, n);
}

static xf_Value csy_run_lines(xf_Value *args, size_t argc) {
    NEED(1);
    const char *cmd; size_t cmdlen;
    if (!arg_str(args, argc, 0, &cmd, &cmdlen)) return propagate(args, argc);
    FILE *fp = popen(cmd, "r");
    if (!fp) return xf_val_nav(XF_TYPE_ARR);
    xf_arr_t *a = xf_arr_new();
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r')) line[--ln] = '\0';
        xf_Str *ls = xf_str_new(line, ln);
        xf_Value lv = xf_val_ok_str(ls); xf_str_release(ls);
        xf_arr_push(a, lv);
    }
    pclose(fp);
    xf_Value r = xf_val_ok_arr(a); xf_arr_release(a); return r;
}

static xf_module_t *build_os(void) {
    xf_module_t *m = xf_module_new("core.os");

    FN("exec",      XF_TYPE_NUM,  csy_exec);
    FN("exit",      XF_TYPE_VOID, csy_exit);
    FN("time",      XF_TYPE_NUM,  csy_time);
    FN("env",       XF_TYPE_STR,  csy_env);
    FN("read",      XF_TYPE_STR,  csy_read);
    FN("write",     XF_TYPE_NUM,  csy_write);
    FN("append",    XF_TYPE_NUM,  csy_append);
    FN("lines",     XF_TYPE_ARR,  csy_lines);
    FN("run",       XF_TYPE_STR,  csy_run);
    FN("run_lines", XF_TYPE_ARR,  csy_run_lines);

    return m;
}


/* ============================================================
 * core.regex
 *
 * Uses POSIX extended regex (regex.h / REG_EXTENDED).
 * All functions accept a pattern string or a /pattern/flags
 * literal (which at runtime is a str with the raw pattern text,
 * since EXPR_REGEX currently returns the pattern string).
 *
 * Flags are encoded as a uint32 stored in the xf_Value's
 * .data.re->flags field when a true regex value is passed, or
 * parsed from a trailing "i", "m", "g" suffix string arg.
 *
 * API:
 *   core.regex.match(str, pattern [, flags_str])
 *     → map {match, index, groups: arr} or NAV on no match
 *
 *   core.regex.search(str, pattern [, flags_str])
 *     → arr of match maps (all non-overlapping matches)
 *       each map: {match, index, groups: arr}
 *
 *   core.regex.replace(str, pattern, replacement [, flags_str])
 *     → str with first match replaced (back-refs \1..\9 supported)
 *
 *   core.regex.replace_all(str, pattern, replacement [, flags_str])
 *     → str with all matches replaced
 *
 *   core.regex.groups(str, pattern [, flags_str])
 *     → arr of capture group strings for first match, or NAV
 *
 *   core.regex.test(str, pattern [, flags_str])
 *     → 1 if pattern matches anywhere in str, else 0
 *
 *   core.regex.split(str, pattern [, flags_str])
 *     → arr of substrings split on the pattern
 * ============================================================ */

#include <regex.h>

/* ── internal helpers ──────────────────────────────────────── */

/* parse flags from a trailing string arg, e.g. "gim" */
static int cr_parse_flags(xf_Value *args, size_t argc, size_t flag_idx) {
    int cflags = REG_EXTENDED;
    if (flag_idx < argc && args[flag_idx].state == XF_STATE_OK &&
        args[flag_idx].type == XF_TYPE_STR && args[flag_idx].data.str) {
        const char *fs = args[flag_idx].data.str->data;
        for (; *fs; fs++) {
            if (*fs == 'i' || *fs == 'I') cflags |= REG_ICASE;
            if (*fs == 'm' || *fs == 'M') cflags |= REG_NEWLINE;
        }
    }
    return cflags;
}

/* compile regex; returns true on success, fills errmsg on failure */
static bool cr_compile(const char *pat, int cflags,
                        regex_t *out, char *errmsg, size_t errmsg_len) {
    int rc = regcomp(out, pat, cflags);
    if (rc != 0) {
        regerror(rc, out, errmsg, errmsg_len);
        regfree(out);
        return false;
    }
    return true;
}

#define CR_MAX_GROUPS 32

/* build a single match-result map from a subject string + regmatch array */
static xf_Value cr_build_match_map(const char *subject, regmatch_t *pm,
                                    size_t ngroups) {
    xf_map_t *m = xf_map_new();

    /* .match — the full match string */
    regmatch_t *full = &pm[0];
    xf_Str *mkey = xf_str_from_cstr("match");
    xf_Str *mval_s = xf_str_new(subject + full->rm_so,
                                  (size_t)(full->rm_eo - full->rm_so));
    xf_map_set(m, mkey, xf_val_ok_str(mval_s));
    xf_str_release(mkey);
    xf_str_release(mval_s);

    /* .index — byte offset of the match */
    xf_Str *ikey = xf_str_from_cstr("index");
    xf_map_set(m, ikey, xf_val_ok_num((double)full->rm_so));
    xf_str_release(ikey);

    /* .groups — arr of capture group strings (empty str for unmatched groups) */
    xf_arr_t *grp_arr = xf_arr_new();
    for (size_t g = 1; g < ngroups; g++) {
        if (pm[g].rm_so >= 0) {
            xf_Str *gs = xf_str_new(subject + pm[g].rm_so,
                                     (size_t)(pm[g].rm_eo - pm[g].rm_so));
            xf_arr_push(grp_arr, xf_val_ok_str(gs));
            xf_str_release(gs);
        } else {
            xf_Str *empty = xf_str_from_cstr("");
            xf_arr_push(grp_arr, xf_val_ok_str(empty));
            xf_str_release(empty);
        }
    }
    xf_Str *gkey = xf_str_from_cstr("groups");
    xf_map_set(m, gkey, xf_val_ok_arr(grp_arr));
    xf_str_release(gkey);
    xf_arr_release(grp_arr);

    return xf_val_ok_map(m);
}

/* apply a replacement string with \1..\9 back-references */
static xf_Str *cr_apply_replacement(const char *subject, regmatch_t *pm,
                                     size_t ngroups, const char *repl) {
    char buf[8192];
    size_t out = 0;
    for (const char *r = repl; *r && out < sizeof(buf) - 1; r++) {
        if (*r == '\\' && *(r+1) >= '1' && *(r+1) <= '9') {
            size_t g = (size_t)(*(r+1) - '0');
            r++;
            if (g < ngroups && pm[g].rm_so >= 0) {
                size_t glen = (size_t)(pm[g].rm_eo - pm[g].rm_so);
                if (out + glen < sizeof(buf) - 1) {
                    memcpy(buf + out, subject + pm[g].rm_so, glen);
                    out += glen;
                }
            }
        } else {
            buf[out++] = *r;
        }
    }
    buf[out] = '\0';
    return xf_str_new(buf, out);
}

/* ── cr_match ──────────────────────────────────────────────── */
/* match(str, pattern [, flags]) → map or NAV */
static xf_Value cr_match(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value sv = xf_coerce_str(args[0]);
    xf_Value pv = xf_coerce_str(args[1]);
    if (sv.state != XF_STATE_OK || pv.state != XF_STATE_OK)
        return xf_val_nav(XF_TYPE_MAP);

    int cflags = cr_parse_flags(args, argc, 2);
    regex_t re;
    char errmsg[256];
    if (!cr_compile(pv.data.str->data, cflags, &re, errmsg, sizeof(errmsg)))
        return xf_val_nav(XF_TYPE_MAP);

    size_t ngroups = re.re_nsub + 1;
    if (ngroups > CR_MAX_GROUPS) ngroups = CR_MAX_GROUPS;
    regmatch_t pm[CR_MAX_GROUPS];

    const char *subject = sv.data.str->data;
    int rc = regexec(&re, subject, ngroups, pm, 0);
    regfree(&re);

    if (rc != 0) return xf_val_nav(XF_TYPE_MAP);
    return cr_build_match_map(subject, pm, ngroups);
}

/* ── cr_search ─────────────────────────────────────────────── */
/* search(str, pattern [, flags]) → arr of match maps */
static xf_Value cr_search(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value sv = xf_coerce_str(args[0]);
    xf_Value pv = xf_coerce_str(args[1]);
    if (sv.state != XF_STATE_OK || pv.state != XF_STATE_OK)
        return xf_val_nav(XF_TYPE_ARR);

    int cflags = cr_parse_flags(args, argc, 2);
    regex_t re;
    char errmsg[256];
    if (!cr_compile(pv.data.str->data, cflags, &re, errmsg, sizeof(errmsg)))
        return xf_val_nav(XF_TYPE_ARR);

    size_t ngroups = re.re_nsub + 1;
    if (ngroups > CR_MAX_GROUPS) ngroups = CR_MAX_GROUPS;
    regmatch_t pm[CR_MAX_GROUPS];

    xf_arr_t *results = xf_arr_new();
    const char *cursor = sv.data.str->data;
    size_t base_offset = 0;

    while (*cursor) {
        int rc = regexec(&re, cursor, ngroups, pm, base_offset ? REG_NOTBOL : 0);
        if (rc != 0) break;

        /* adjust match map index to be absolute */
        regmatch_t abs_pm[CR_MAX_GROUPS];
        for (size_t g = 0; g < ngroups; g++) {
            abs_pm[g] = pm[g];
            if (pm[g].rm_so >= 0) {
                abs_pm[g].rm_so += (regoff_t)base_offset;
                abs_pm[g].rm_eo += (regoff_t)base_offset;
            }
        }

        xf_Value mv = cr_build_match_map(sv.data.str->data, abs_pm, ngroups);
        xf_arr_push(results, mv);

        /* advance past this match (at least 1 char to avoid infinite loop) */
        size_t adv = (pm[0].rm_eo > pm[0].rm_so)
                     ? (size_t)pm[0].rm_eo : 1;
        base_offset += adv;
        cursor      += adv;
    }

    regfree(&re);
    xf_Value rv = xf_val_ok_arr(results);
    xf_arr_release(results);
    return rv;
}

/* ── cr_replace_impl ───────────────────────────────────────── */
static xf_Value cr_replace_impl(xf_Value *args, size_t argc, bool global) {
    NEED(3);
    xf_Value sv   = xf_coerce_str(args[0]);
    xf_Value pv   = xf_coerce_str(args[1]);
    xf_Value repv = xf_coerce_str(args[2]);
    if (sv.state != XF_STATE_OK || pv.state != XF_STATE_OK || repv.state != XF_STATE_OK)
        return xf_val_nav(XF_TYPE_STR);

    int cflags = cr_parse_flags(args, argc, 3);
    regex_t re;
    char errmsg[256];
    if (!cr_compile(pv.data.str->data, cflags, &re, errmsg, sizeof(errmsg)))
        return xf_val_nav(XF_TYPE_STR);

    size_t ngroups = re.re_nsub + 1;
    if (ngroups > CR_MAX_GROUPS) ngroups = CR_MAX_GROUPS;
    regmatch_t pm[CR_MAX_GROUPS];

    const char *subject = sv.data.str->data;
    const char *repl    = repv.data.str->data;

    /* build result in a dynamic buffer */
    size_t cap = strlen(subject) * 2 + 256;
    char  *out = malloc(cap);
    size_t used = 0;

#define ENSURE(n) \
    do { if (used + (n) + 1 >= cap) { cap = cap * 2 + (n) + 1; out = realloc(out, cap); } } while(0)

    const char *cursor = subject;
    int eflags = 0;
    while (*cursor) {
        int rc = regexec(&re, cursor, ngroups, pm, eflags);
        if (rc != 0) break;

        /* copy pre-match literal */
        size_t pre = (size_t)pm[0].rm_so;
        ENSURE(pre);
        memcpy(out + used, cursor, pre);
        used += pre;

        /* apply replacement */
        xf_Str *rep_str = cr_apply_replacement(cursor, pm, ngroups, repl);
        ENSURE(rep_str->len);
        memcpy(out + used, rep_str->data, rep_str->len);
        used += rep_str->len;
        xf_str_release(rep_str);

        /* advance */
        size_t adv = (pm[0].rm_eo > pm[0].rm_so) ? (size_t)pm[0].rm_eo : 1;
        if (adv == 0) {
            /* zero-width match — copy one char to avoid infinite loop */
            ENSURE(1);
            out[used++] = *cursor;
            adv = 1;
        }
        cursor += adv;
        eflags  = REG_NOTBOL;

        if (!global) break;
    }

#undef ENSURE

    /* copy remainder */
    size_t tail = strlen(cursor);
    if (used + tail + 1 >= cap) { cap = used + tail + 2; out = realloc(out, cap); }
    memcpy(out + used, cursor, tail);
    used += tail;
    out[used] = '\0';

    regfree(&re);
    xf_Str *result = xf_str_new(out, used);
    free(out);
    xf_Value rv = xf_val_ok_str(result);
    xf_str_release(result);
    return rv;
}

/* ── cr_replace / cr_replace_all ───────────────────────────── */
static xf_Value cr_replace(xf_Value *args, size_t argc) {
    return cr_replace_impl(args, argc, false);
}
static xf_Value cr_replace_all(xf_Value *args, size_t argc) {
    return cr_replace_impl(args, argc, true);
}

/* ── cr_groups ─────────────────────────────────────────────── */
/* groups(str, pattern [, flags]) → arr of capture group strings */
static xf_Value cr_groups(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value sv = xf_coerce_str(args[0]);
    xf_Value pv = xf_coerce_str(args[1]);
    if (sv.state != XF_STATE_OK || pv.state != XF_STATE_OK)
        return xf_val_nav(XF_TYPE_ARR);

    int cflags = cr_parse_flags(args, argc, 2);
    regex_t re;
    char errmsg[256];
    if (!cr_compile(pv.data.str->data, cflags, &re, errmsg, sizeof(errmsg)))
        return xf_val_nav(XF_TYPE_ARR);

    size_t ngroups = re.re_nsub + 1;
    if (ngroups > CR_MAX_GROUPS) ngroups = CR_MAX_GROUPS;
    regmatch_t pm[CR_MAX_GROUPS];

    const char *subject = sv.data.str->data;
    int rc = regexec(&re, subject, ngroups, pm, 0);
    regfree(&re);

    if (rc != 0) return xf_val_nav(XF_TYPE_ARR);

    xf_arr_t *arr = xf_arr_new();
    for (size_t g = 1; g < ngroups; g++) {
        if (pm[g].rm_so >= 0) {
            xf_Str *gs = xf_str_new(subject + pm[g].rm_so,
                                     (size_t)(pm[g].rm_eo - pm[g].rm_so));
            xf_arr_push(arr, xf_val_ok_str(gs));
            xf_str_release(gs);
        } else {
            xf_Str *empty = xf_str_from_cstr("");
            xf_arr_push(arr, xf_val_ok_str(empty));
            xf_str_release(empty);
        }
    }
    xf_Value rv = xf_val_ok_arr(arr);
    xf_arr_release(arr);
    return rv;
}

/* ── cr_test ───────────────────────────────────────────────── */
/* test(str, pattern [, flags]) → 1 or 0 */
static xf_Value cr_test(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value sv = xf_coerce_str(args[0]);
    xf_Value pv = xf_coerce_str(args[1]);
    if (sv.state != XF_STATE_OK || pv.state != XF_STATE_OK)
        return xf_val_ok_num(0);

    int cflags = cr_parse_flags(args, argc, 2);
    regex_t re;
    char errmsg[256];
    if (!cr_compile(pv.data.str->data, cflags, &re, errmsg, sizeof(errmsg)))
        return xf_val_ok_num(0);

    int rc = regexec(&re, sv.data.str->data, 0, NULL, 0);
    regfree(&re);
    return xf_val_ok_num(rc == 0 ? 1.0 : 0.0);
}

/* ── cr_split ──────────────────────────────────────────────── */
/* split(str, pattern [, flags]) → arr of substrings */
static xf_Value cr_split(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value sv = xf_coerce_str(args[0]);
    xf_Value pv = xf_coerce_str(args[1]);
    if (sv.state != XF_STATE_OK || pv.state != XF_STATE_OK)
        return xf_val_nav(XF_TYPE_ARR);

    int cflags = cr_parse_flags(args, argc, 2);
    regex_t re;
    char errmsg[256];
    if (!cr_compile(pv.data.str->data, cflags, &re, errmsg, sizeof(errmsg)))
        return xf_val_nav(XF_TYPE_ARR);

    regmatch_t pm[1];
    xf_arr_t *arr    = xf_arr_new();
    const char *cursor = sv.data.str->data;

    while (*cursor) {
        int rc = regexec(&re, cursor, 1, pm, 0);
        if (rc != 0) break;

        /* segment before the match */
        xf_Str *seg = xf_str_new(cursor, (size_t)pm[0].rm_so);
        xf_arr_push(arr, xf_val_ok_str(seg));
        xf_str_release(seg);

        size_t adv = (pm[0].rm_eo > pm[0].rm_so) ? (size_t)pm[0].rm_eo : 1;
        cursor += adv;
    }
    /* remainder */
    xf_Str *tail = xf_str_from_cstr(cursor);
    xf_arr_push(arr, xf_val_ok_str(tail));
    xf_str_release(tail);

    regfree(&re);
    xf_Value rv = xf_val_ok_arr(arr);
    xf_arr_release(arr);
    return rv;
}

/* ── build_regex ───────────────────────────────────────────── */
static xf_module_t *build_regex(void) {
    xf_module_t *m = xf_module_new("core.regex");
    FN("match",       XF_TYPE_MAP, cr_match);
    FN("search",      XF_TYPE_ARR, cr_search);
    FN("replace",     XF_TYPE_STR, cr_replace);
    FN("replace_all", XF_TYPE_STR, cr_replace_all);
    FN("groups",      XF_TYPE_ARR, cr_groups);
    FN("test",        XF_TYPE_NUM, cr_test);
    FN("split",       XF_TYPE_ARR, cr_split);
    return m;
}

void core_register(SymTable *st) {
    xf_module_t *math_m     = build_math();
    xf_module_t *str_m      = build_str();
    xf_module_t *os_m       = build_os();
    xf_module_t *generics_m = build_generics();
    xf_module_t *ds_m       = build_ds();
    xf_module_t *regex_m    = build_regex();

    /* build the top-level core module */
    xf_module_t *core_m = xf_module_new("core");
    xf_module_set(core_m, "math",     xf_val_ok_module(math_m));
    xf_module_set(core_m, "str",      xf_val_ok_module(str_m));
    xf_module_set(core_m, "os",       xf_val_ok_module(os_m));
    xf_module_set(core_m, "generics", xf_val_ok_module(generics_m));
    xf_module_set(core_m, "ds",       xf_val_ok_module(ds_m));
    xf_module_set(core_m, "regex",    xf_val_ok_module(regex_m));

    /* release local refs — core_m retains them internally */
    xf_module_release(math_m);
    xf_module_release(str_m);
    xf_module_release(os_m);
    xf_module_release(generics_m);
    xf_module_release(ds_m);
    xf_module_release(regex_m);

    /* register core as a global symbol */
    xf_Value core_val = xf_val_ok_module(core_m);
    xf_module_release(core_m);

    xf_Str *name = xf_str_from_cstr("core");
    Symbol *sym  = sym_declare(st, name, SYM_BUILTIN, XF_TYPE_MODULE,
                               (Loc){.source="<core>", .line=0, .col=0});
    if (sym) {
        sym->value      = core_val;
        sym->state      = XF_STATE_OK;
        sym->is_const   = true;
        sym->is_defined = true;
    }
    xf_str_release(name);
}

/* ============================================================
 * core.ds — dataset functions (arr-of-map row model)
 * ============================================================
 *
 * A "dataset" is an xf_arr of xf_maps, where each map is a row
 * and map keys are column names.  All ds_ functions are
 * non-mutating; they return new values.
 * ============================================================ */

/* ── helpers ──────────────────────────────────────────────── */

/* get the arr from arg 0; must be XF_TYPE_ARR */
static bool ds_arg_arr(xf_Value *args, size_t argc, size_t i, xf_arr_t **out) {
    if (i >= argc) return false;
    xf_Value v = args[i];
    if (v.state != XF_STATE_OK || v.type != XF_TYPE_ARR || !v.data.arr) return false;
    *out = v.data.arr;
    return true;
}

/* get a row (map) from the dataset at index r; NULL if not a map row */
static xf_map_t *ds_row_map(xf_arr_t *ds, size_t r) {
    if (r >= ds->len) return NULL;
    xf_Value rv = ds->items[r];
    if (rv.state != XF_STATE_OK || rv.type != XF_TYPE_MAP || !rv.data.map) return NULL;
    return rv.data.map;
}

/* get cell value from a map row by string key */
static xf_Value ds_cell(xf_map_t *row, const char *key) {
    xf_Str *ks = xf_str_from_cstr(key);
    xf_Value v = xf_map_get(row, ks);
    xf_str_release(ks);
    return v;
}

/* compare two values as strings for sorting */
static int ds_val_cmp_str(xf_Value a, xf_Value b) {
    xf_Value as = xf_coerce_str(a);
    xf_Value bs = xf_coerce_str(b);
    const char *ap = (as.state == XF_STATE_OK && as.data.str) ? as.data.str->data : "";
    const char *bp = (bs.state == XF_STATE_OK && bs.data.str) ? bs.data.str->data : "";
    int r = strcmp(ap, bp);
    return r;
}

/* compare two values numerically if both are nums, else as strings */
static int ds_val_cmp(xf_Value a, xf_Value b) {
    if (a.state == XF_STATE_OK && a.type == XF_TYPE_NUM &&
        b.state == XF_STATE_OK && b.type == XF_TYPE_NUM) {
        if (a.data.num < b.data.num) return -1;
        if (a.data.num > b.data.num) return  1;
        return 0;
    }
    return ds_val_cmp_str(a, b);
}

/* ── column(ds, name) → arr ──────────────────────────────── */
/* Extract one column from the dataset as an array of values. */
static xf_Value cd_column(xf_Value *args, size_t argc) {
    NEED(2);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);
    const char *col; size_t clen;
    if (!arg_str(args, argc, 1, &col, &clen)) return propagate(args, argc);

    xf_arr_t *out = xf_arr_new();
    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) { xf_arr_push(out, xf_val_nav(XF_TYPE_VOID)); continue; }
        xf_arr_push(out, ds_cell(row, col));
    }
    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}

/* ── row(ds, i) → map ─────────────────────────────────────── */
/* Return the map at row index i (0-based).  NAV if out of range. */
static xf_Value cd_row(xf_Value *args, size_t argc) {
    NEED(2);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);
    double di;
    if (!arg_num(args, argc, 1, &di)) return propagate(args, argc);
    size_t idx = (size_t)(di < 0 ? 0 : di);
    if (idx >= ds->len) return xf_val_nav(XF_TYPE_MAP);
    return ds->items[idx];
}

/* ── sort(ds, key [, "desc"]) → arr ─────────────────────── */
/* Sort rows by the named column.  Default ascending; pass "desc"
   as the third argument for descending order. */

typedef struct {
    const char *key;
    int         dir;  /* +1 = asc, -1 = desc */
} SortCtx;

static SortCtx g_sort_ctx;

static int ds_sort_cmp(const void *a, const void *b) {
    const xf_Value *ra = (const xf_Value *)a;
    const xf_Value *rb = (const xf_Value *)b;
    xf_map_t *ma = (ra->state == XF_STATE_OK && ra->type == XF_TYPE_MAP) ? ra->data.map : NULL;
    xf_map_t *mb = (rb->state == XF_STATE_OK && rb->type == XF_TYPE_MAP) ? rb->data.map : NULL;
    xf_Value va = ma ? ds_cell(ma, g_sort_ctx.key) : xf_val_nav(XF_TYPE_VOID);
    xf_Value vb = mb ? ds_cell(mb, g_sort_ctx.key) : xf_val_nav(XF_TYPE_VOID);
    return g_sort_ctx.dir * ds_val_cmp(va, vb);
}

static xf_Value cd_sort(xf_Value *args, size_t argc) {
    NEED(2);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);
    const char *col; size_t clen;
    if (!arg_str(args, argc, 1, &col, &clen)) return propagate(args, argc);

    int dir = 1;
    if (argc >= 3) {
        const char *d; size_t dlen;
        if (arg_str(args, argc, 2, &d, &dlen) && strncmp(d, "desc", 4) == 0)
            dir = -1;
    }

    /* copy the items array so we can sort without mutating the original */
    xf_arr_t *out = xf_arr_new();
    for (size_t i = 0; i < ds->len; i++) xf_arr_push(out, ds->items[i]);

    g_sort_ctx.key = col;
    g_sort_ctx.dir = dir;
    qsort(out->items, out->len, sizeof(xf_Value), ds_sort_cmp);

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}

/* ── agg(ds, group_key, agg_key) → map of arr ─────────────── */
/* Group rows by group_key; collect agg_key values for each group
   into an array.  Returns a map: group_value → arr of agg values.
   If agg_key is omitted, each group maps to an array of full rows. */
static xf_Value cd_agg(xf_Value *args, size_t argc) {
    NEED(2);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);
    const char *gkey; size_t glen;
    if (!arg_str(args, argc, 1, &gkey, &glen)) return propagate(args, argc);
    const char *akey = NULL; size_t alen = 0;
    bool has_akey = (argc >= 3 && arg_str(args, argc, 2, &akey, &alen));

    xf_map_t *out = xf_map_new();
    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) continue;

        xf_Value gval = ds_cell(row, gkey);
        xf_Value gs   = xf_coerce_str(gval);
        if (gs.state != XF_STATE_OK || !gs.data.str) continue;
        xf_Str *gstr = gs.data.str;

        /* get or create the bucket array */
        xf_Value bucket = xf_map_get(out, gstr);
        xf_arr_t *ba;
        if (bucket.state != XF_STATE_OK || bucket.type != XF_TYPE_ARR || !bucket.data.arr) {
            ba = xf_arr_new();
            xf_Value bv = xf_val_ok_arr(ba);
            xf_map_set(out, gstr, bv);
            xf_arr_release(ba);
            bucket = xf_map_get(out, gstr);
            ba = bucket.data.arr;
        } else {
            ba = bucket.data.arr;
        }

        xf_Value push_val = has_akey ? ds_cell(row, akey) : ds->items[i];
        xf_arr_push(ba, push_val);
    }

    xf_Value rv = xf_val_ok_map(out);
    return rv;
}

/* ── merge(ds1, ds2 [, key]) → arr ──────────────────────── */
/* With 2 args: concatenate ds1 and ds2 row-by-row.
   With 3 args (key): left-join ds1 with ds2 on key — each row in
   ds1 is merged with the first matching row from ds2 (if any). */
static xf_Value cd_merge(xf_Value *args, size_t argc) {
    NEED(2);
    xf_arr_t *ds1, *ds2;
    if (!ds_arg_arr(args, argc, 0, &ds1)) return propagate(args, argc);
    if (!ds_arg_arr(args, argc, 1, &ds2)) return propagate(args, argc);

    const char *jkey = NULL; size_t jlen = 0;
    bool join_mode = (argc >= 3 && arg_str(args, argc, 2, &jkey, &jlen));

    xf_arr_t *out = xf_arr_new();

    if (!join_mode) {
        /* simple concat */
        for (size_t i = 0; i < ds1->len; i++) xf_arr_push(out, ds1->items[i]);
        for (size_t i = 0; i < ds2->len; i++) xf_arr_push(out, ds2->items[i]);
    } else {
        /* left join on jkey: for each row in ds1 find matching row in ds2 */
        for (size_t i = 0; i < ds1->len; i++) {
            xf_map_t *r1 = ds_row_map(ds1, i);
            if (!r1) { xf_arr_push(out, ds1->items[i]); continue; }

            xf_Value jv1 = ds_cell(r1, jkey);
            xf_Value js1 = xf_coerce_str(jv1);

            xf_map_t *matched = NULL;
            if (js1.state == XF_STATE_OK && js1.data.str) {
                for (size_t j = 0; j < ds2->len; j++) {
                    xf_map_t *r2 = ds_row_map(ds2, j);
                    if (!r2) continue;
                    xf_Value jv2 = ds_cell(r2, jkey);
                    xf_Value js2 = xf_coerce_str(jv2);
                    if (js2.state == XF_STATE_OK && js2.data.str &&
                        strcmp(js1.data.str->data, js2.data.str->data) == 0) {
                        matched = r2;
                        break;
                    }
                }
            }

            if (!matched) {
                /* no match — just include the ds1 row as-is */
                xf_arr_push(out, ds1->items[i]);
            } else {
                /* merge row maps: start with all keys from r1, overlay r2 */
                xf_map_t *merged = xf_map_new();
                for (size_t k = 0; k < r1->order_len; k++)
                    xf_map_set(merged, r1->order[k], xf_map_get(r1, r1->order[k]));
                for (size_t k = 0; k < matched->order_len; k++)
                    xf_map_set(merged, matched->order[k], xf_map_get(matched, matched->order[k]));
                xf_Value mv = xf_val_ok_map(merged);
                xf_arr_push(out, mv);
                (void)merged; /* map not retained by val_ok_map; do not release */
            }
        }
    }

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}

/* ── index(ds, key) → map ─────────────────────────────────── */
/* Build a map from key-column value → first matching row (map).
   Useful for O(1) row lookups after indexing. */
static xf_Value cd_index(xf_Value *args, size_t argc) {
    NEED(2);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);
    const char *col; size_t clen;
    if (!arg_str(args, argc, 1, &col, &clen)) return propagate(args, argc);

    xf_map_t *out = xf_map_new();
    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) continue;
        xf_Value kv = ds_cell(row, col);
        xf_Value ks = xf_coerce_str(kv);
        if (ks.state != XF_STATE_OK || !ks.data.str) continue;
        /* only insert if key not already present (first-wins) */
        xf_Value existing = xf_map_get(out, ks.data.str);
        if (existing.state != XF_STATE_OK)
            xf_map_set(out, ks.data.str, ds->items[i]);
    }
    xf_Value v = xf_val_ok_map(out);
    return v;
}

/* ── keys(ds) → arr ───────────────────────────────────────── */
/* Return the unique column names across all rows, in first-seen order. */
static xf_Value cd_keys(xf_Value *args, size_t argc) {
    NEED(1);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    /* use a map as a seen-set, build result arr in insertion order */
    xf_map_t *seen = xf_map_new();
    xf_arr_t *out  = xf_arr_new();
    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) continue;
        for (size_t k = 0; k < row->order_len; k++) {
            xf_Str *kname = row->order[k];
            xf_Value existing = xf_map_get(seen, kname);
            if (existing.state != XF_STATE_OK) {
                xf_map_set(seen, kname, xf_val_ok_num(1.0));
                xf_arr_push(out, xf_val_ok_str(kname));
            }
        }
    }
    xf_map_release(seen);
    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}

/* ── values(ds [, key]) → arr ─────────────────────────────── */
/* No key: return each row as an array of its values (in key-insertion order).
   With key: equivalent to column(ds, key). */
static xf_Value cd_values(xf_Value *args, size_t argc) {
    NEED(1);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    /* with key — delegate to column */
    if (argc >= 2 && args[1].state == XF_STATE_OK && args[1].type == XF_TYPE_STR)
        return cd_column(args, argc);

    /* without key — arr of arr */
    xf_arr_t *out = xf_arr_new();
    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) { xf_arr_push(out, xf_val_nav(XF_TYPE_ARR)); continue; }
        xf_arr_t *rv = xf_arr_new();
        for (size_t k = 0; k < row->order_len; k++)
            xf_arr_push(rv, xf_map_get(row, row->order[k]));
        xf_Value vv = xf_val_ok_arr(rv);
        xf_arr_push(out, vv);
        xf_arr_release(rv);
    }
    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}

/* ── filter(ds, key, val) → arr ──────────────────────────── */
/* Return rows where row[key] == val (string comparison).
   val may also be a num; both sides are coerced to str for comparison.
   Omitting val returns rows where the key exists and is non-empty. */
static xf_Value cd_filter(xf_Value *args, size_t argc) {
    NEED(2);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);
    const char *col; size_t clen;
    if (!arg_str(args, argc, 1, &col, &clen)) return propagate(args, argc);

    bool has_val = (argc >= 3 && args[2].state == XF_STATE_OK);
    xf_Value match_vs = has_val ? xf_coerce_str(args[2]) : xf_val_nav(XF_TYPE_VOID);
    const char *match_s = (has_val && match_vs.state == XF_STATE_OK && match_vs.data.str)
                           ? match_vs.data.str->data : NULL;

    xf_arr_t *out = xf_arr_new();
    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) continue;
        xf_Value cell = ds_cell(row, col);
        if (!has_val) {
            /* existence check: cell must be OK and non-empty string */
            if (cell.state != XF_STATE_OK) continue;
            xf_Value cs = xf_coerce_str(cell);
            if (cs.state != XF_STATE_OK || !cs.data.str || cs.data.str->len == 0) continue;
            xf_arr_push(out, ds->items[i]);
        } else {
            xf_Value cs = xf_coerce_str(cell);
            if (cs.state != XF_STATE_OK || !cs.data.str) continue;
            if (match_s && strcmp(cs.data.str->data, match_s) == 0)
                xf_arr_push(out, ds->items[i]);
        }
    }
    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}

/* ── transpose(ds) → map of arr ──────────────────────────── */
/* Pivot the dataset: returns a map of column_name → arr of values.
   Equivalent to collecting every column into a named array. */
static xf_Value cd_transpose(xf_Value *args, size_t argc) {
    NEED(1);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    /* collect all column names in first-seen order */
    xf_map_t *seen  = xf_map_new();
    xf_arr_t *cols  = xf_arr_new();
    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) continue;
        for (size_t k = 0; k < row->order_len; k++) {
            xf_Str *kname = row->order[k];
            if (xf_map_get(seen, kname).state != XF_STATE_OK) {
                xf_map_set(seen, kname, xf_val_ok_num(1.0));
                xf_arr_push(cols, xf_val_ok_str(kname));
            }
        }
    }
    xf_map_release(seen);

    /* build result map: col_name → arr of values */
    xf_map_t *out = xf_map_new();
    for (size_t c = 0; c < cols->len; c++) {
        xf_Str *cname = cols->items[c].data.str;
        xf_arr_t *col_arr = xf_arr_new();
        for (size_t i = 0; i < ds->len; i++) {
            xf_map_t *row = ds_row_map(ds, i);
            xf_Value cell = row ? ds_cell(row, cname->data) : xf_val_nav(XF_TYPE_VOID);
            xf_arr_push(col_arr, cell);
        }
        xf_Value cv = xf_val_ok_arr(col_arr);
        xf_map_set(out, cname, cv);
        xf_arr_release(col_arr);
    }
    xf_arr_release(cols);

    xf_Value v = xf_val_ok_map(out);
    return v;
}

static xf_module_t *build_ds(void) {
    xf_module_t *m = xf_module_new("core.ds");
    FN("column",    XF_TYPE_ARR, cd_column);
    FN("row",       XF_TYPE_MAP, cd_row);
    FN("sort",      XF_TYPE_ARR, cd_sort);
    FN("agg",       XF_TYPE_MAP, cd_agg);
    FN("merge",     XF_TYPE_ARR, cd_merge);
    FN("index",     XF_TYPE_MAP, cd_index);
    FN("keys",      XF_TYPE_ARR, cd_keys);
    FN("values",    XF_TYPE_ARR, cd_values);
    FN("filter",    XF_TYPE_ARR, cd_filter);
    FN("transpose", XF_TYPE_MAP, cd_transpose);
    return m;
}