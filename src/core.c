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
#define FN(name, ret, impl) \
    xf_module_set(m, name, xf_val_native_fn(name, ret, impl))
/* require at least n args; return NAV if fewer given */
#define NEED(n) \
    do { if (argc < (n)) return xf_val_nav(XF_TYPE_VOID); } while(0)
static xf_Value make_str_val(const char *data, size_t len);
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

/* trim helpers */
static xf_Value make_str_val(const char *data, size_t len) {
    xf_Str *s = xf_str_new(data, len);
    xf_Value v = xf_val_ok_str(s);
    xf_str_release(s);
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
 * core.generics native functions
 * ============================================================ */

/* ── join ─────────────────────────────────────────────────── */
/* join(collection, sep)
 *   arr/set  → join elements as strings with sep
 *   map      → join insertion-ordered values as strings with sep
 *   str      → identical to concat(a, sep, b) — joins two strings
 *   num      → coerce both operands to str, join, try to re-coerce
 *              to num; if that fails emit a warning and return str */
static xf_Value cg_join(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value coll = (argc >= 1) ? args[0] : xf_val_nav(XF_TYPE_VOID);
    if (coll.state != XF_STATE_OK) return coll;

    const char *sep; size_t seplen;
    if (!arg_str(args, argc, 1, &sep, &seplen)) return propagate(args, argc);

    /* ── num: coerce both sides, join, try re-coerce ── */
    if (coll.type == XF_TYPE_NUM) {
        if (argc < 3) return propagate(args, argc);
        xf_Value as = xf_coerce_str(coll);
        xf_Value bs = xf_coerce_str(args[2]);
        if (as.state != XF_STATE_OK || bs.state != XF_STATE_OK)
            return propagate(args, argc);
        size_t alen = as.data.str->len;
        size_t blen = bs.data.str->len;
        size_t total = alen + seplen + blen;
        char *buf = malloc(total + 1);
        memcpy(buf,              as.data.str->data, alen);
        memcpy(buf + alen,       sep,               seplen);
        memcpy(buf + alen + seplen, bs.data.str->data, blen);
        buf[total] = '\0';
        xf_Value joined = make_str_val(buf, total);
        free(buf);
        xf_Value num_try = xf_coerce_num(joined);
        if (num_try.state == XF_STATE_OK) return num_try;
        /* implicit warning: result cannot be cast back to num, returning str */
        fprintf(stderr, "xf warning: join of num values produced non-numeric "
                        "result, returning str\n");
        return joined;
    }

    /* ── str: simple two-string join ── */
    if (coll.type == XF_TYPE_STR) {
        if (argc < 3) {
            /* just return the string unchanged if no second operand */
            return coll;
        }
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

    /* ── arr ── */
    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        xf_arr_t *a = coll.data.arr;
        /* first pass: measure total length */
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
            if (i + 1 < a->len) {
                memcpy(buf + pos, sep, seplen);
                pos += seplen;
            }
        }
        buf[pos] = '\0';
        xf_Value v = make_str_val(buf, pos);
        free(buf);
        return v;
    }

    /* ── map: join insertion-ordered values ── */
    if ((coll.type == XF_TYPE_MAP || coll.type == XF_TYPE_SET) && coll.data.map) {
        xf_map_t *m = coll.data.map;
        size_t total = 0;
        for (size_t i = 0; i < m->order_len; i++) {
            xf_Value val = xf_map_get(m, m->order[i]);
            /* for sets, the key itself is the member */
            xf_Value sv;
            if (coll.type == XF_TYPE_SET)
                sv = xf_val_ok_str(m->order[i]);
            else
                sv = xf_coerce_str(val);
            if (sv.state == XF_STATE_OK && sv.data.str)
                total += sv.data.str->len;
            if (i + 1 < m->order_len) total += seplen;
        }
        char *buf = malloc(total + 1);
        size_t pos = 0;
        for (size_t i = 0; i < m->order_len; i++) {
            xf_Value val = xf_map_get(m, m->order[i]);
            xf_Value sv;
            if (coll.type == XF_TYPE_SET)
                sv = xf_val_ok_str(m->order[i]);
            else
                sv = xf_coerce_str(val);
            if (sv.state == XF_STATE_OK && sv.data.str) {
                memcpy(buf + pos, sv.data.str->data, sv.data.str->len);
                pos += sv.data.str->len;
            }
            if (i + 1 < m->order_len) {
                memcpy(buf + pos, sep, seplen);
                pos += seplen;
            }
        }
        buf[pos] = '\0';
        xf_Value v = make_str_val(buf, pos);
        free(buf);
        return v;
    }

    return xf_val_nav(XF_TYPE_STR);
}

/* ── split ────────────────────────────────────────────────── */
/* split(str, sep)  → arr of substrings
 * split(arr, pred) → [passing, failing] where pred is a str key check
 *                    (arr split is a future extension; for now returns NAV) */
static xf_Value cg_split(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value src = args[0];
    if (src.state != XF_STATE_OK) return src;

    /* arr split placeholder — not yet implemented */
    if (src.type == XF_TYPE_ARR || src.type == XF_TYPE_MAP ||
        src.type == XF_TYPE_SET) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    /* str split */
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    const char *sep; size_t seplen;
    if (!arg_str(args, argc, 1, &sep, &seplen)) return propagate(args, argc);

    xf_arr_t *out = xf_arr_new();

    if (seplen == 0) {
        /* split into individual characters */
        for (size_t i = 0; i < slen; i++) {
            xf_Value cv = make_str_val(s + i, 1);
            xf_arr_push(out, cv);
        }
    } else {
        const char *p = s;
        const char *end = s + slen;
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

/* ── strip ────────────────────────────────────────────────── */
/* strip(str)          → trim whitespace from both ends
 * strip(str, chars)   → trim any chars in the set from both ends
 * strip(arr)          → new arr with each element stripped (str elements)
 * strip(map)          → new map with each str value stripped
 * strip(set)          → new set with each member stripped */
static xf_Value cg_strip(xf_Value *args, size_t argc) {
    NEED(1);
    xf_Value v = args[0];
    if (v.state != XF_STATE_OK) return v;

    /* optional char set to strip (defaults to whitespace) */
    const char *chars = NULL; size_t chars_len = 0;
    bool has_chars = (argc >= 2 && args[1].state == XF_STATE_OK &&
                      args[1].type == XF_TYPE_STR);
    if (has_chars) arg_str(args, argc, 1, &chars, &chars_len);

    /* helper: is character in strip set? */
    #define STRIP_CHAR(c)         (has_chars ? (chars_len > 0 && memchr(chars, (unsigned char)(c), chars_len) != NULL)                    : isspace((unsigned char)(c)))

    /* ── str ── */
    if (v.type == XF_TYPE_STR && v.data.str) {
        const char *s = v.data.str->data;
        size_t lo = 0, hi = v.data.str->len;
        while (lo < hi && STRIP_CHAR(s[lo])) lo++;
        while (hi > lo && STRIP_CHAR(s[hi-1])) hi--;
        return make_str_val(s + lo, hi - lo);
    }

    /* ── arr: strip each str element ── */
    if (v.type == XF_TYPE_ARR && v.data.arr) {
        xf_arr_t *src = v.data.arr;
        xf_arr_t *out = xf_arr_new();
        for (size_t i = 0; i < src->len; i++) {
            xf_Value elem = src->items[i];
            if (elem.state == XF_STATE_OK && elem.type == XF_TYPE_STR &&
                elem.data.str) {
                const char *s = elem.data.str->data;
                size_t lo = 0, hi = elem.data.str->len;
                while (lo < hi && STRIP_CHAR(s[lo])) lo++;
                while (hi > lo && STRIP_CHAR(s[hi-1])) hi--;
                xf_arr_push(out, make_str_val(s + lo, hi - lo));
            } else {
                xf_arr_push(out, elem);
            }
        }
        xf_Value rv = xf_val_ok_arr(out);
        xf_arr_release(out);
        return rv;
    }

    /* ── map: strip each str value ── */
    if (v.type == XF_TYPE_MAP && v.data.map) {
        xf_map_t *src = v.data.map;
        xf_map_t *out = xf_map_new();
        for (size_t i = 0; i < src->order_len; i++) {
            xf_Str *key = src->order[i];
            xf_Value val = xf_map_get(src, key);
            if (val.state == XF_STATE_OK && val.type == XF_TYPE_STR &&
                val.data.str) {
                const char *s = val.data.str->data;
                size_t lo = 0, hi = val.data.str->len;
                while (lo < hi && STRIP_CHAR(s[lo])) lo++;
                while (hi > lo && STRIP_CHAR(s[hi-1])) hi--;
                xf_Value sv = make_str_val(s + lo, hi - lo);
                xf_map_set(out, key, sv);
            } else {
                xf_map_set(out, key, val);
            }
        }
        xf_Value rv = xf_val_ok_map(out);
        xf_map_release(out);
        return rv;
    }

    /* ── set: strip each member key ── */
    if (v.type == XF_TYPE_SET && v.data.map) {
        xf_map_t *src = v.data.map;
        xf_map_t *out = xf_map_new();
        for (size_t i = 0; i < src->order_len; i++) {
            xf_Str *key = src->order[i];
            const char *s = key->data;
            size_t lo = 0, hi = key->len;
            while (lo < hi && STRIP_CHAR(s[lo])) lo++;
            while (hi > lo && STRIP_CHAR(s[hi-1])) hi--;
            xf_Str *nk = xf_str_new(s + lo, hi - lo);
            xf_map_set(out, nk, xf_val_ok_num(1.0));
            xf_str_release(nk);
        }
        xf_Value rv = xf_val_ok_map(out);
        rv.type = XF_TYPE_SET;
        xf_map_release(out);
        return rv;
    }

    #undef STRIP_CHAR
    return xf_val_nav(XF_TYPE_VOID);
}

/* ── contains ─────────────────────────────────────────────── */
/* contains(str,   needle) → 1 if needle is a substring
 * contains(arr,   val)    → 1 if any element equals val (string comparison)
 * contains(map,   key)    → 1 if key exists
 * contains(set,   member) → 1 if member is in set */
static xf_Value cg_contains(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value coll  = args[0];
    xf_Value needle = args[1];
    if (coll.state  != XF_STATE_OK) return coll;
    if (needle.state != XF_STATE_OK) return needle;

    /* ── str ── */
    if (coll.type == XF_TYPE_STR) {
        xf_Value ns = xf_coerce_str(needle);
        if (ns.state != XF_STATE_OK || !coll.data.str || !ns.data.str)
            return xf_val_ok_num(0);
        double r = strstr(coll.data.str->data, ns.data.str->data) ? 1.0 : 0.0;
        xf_str_release(ns.data.str);
        return xf_val_ok_num(r);
    }

    /* ── arr ── */
    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        xf_Value ns = xf_coerce_str(needle);
        if (ns.state != XF_STATE_OK || !ns.data.str)
            return xf_val_ok_num(0);
        xf_arr_t *a = coll.data.arr;
        for (size_t i = 0; i < a->len; i++) {
            xf_Value es = xf_coerce_str(a->items[i]);
            if (es.state == XF_STATE_OK && es.data.str) {
                int match = strcmp(es.data.str->data, ns.data.str->data) == 0;
                xf_str_release(es.data.str);
                if (match) { xf_str_release(ns.data.str); return xf_val_ok_num(1.0); }
            }
        }
        xf_str_release(ns.data.str);
        return xf_val_ok_num(0.0);
    }

    /* ── map or set ── */
    if ((coll.type == XF_TYPE_MAP || coll.type == XF_TYPE_SET) && coll.data.map) {
        xf_Value ks = xf_coerce_str(needle);
        if (ks.state != XF_STATE_OK || !ks.data.str) return xf_val_ok_num(0.0);
        xf_Value got = xf_map_get(coll.data.map, ks.data.str);
        xf_str_release(ks.data.str);
        return xf_val_ok_num(got.state == XF_STATE_OK ? 1.0 : 0.0);
    }

    return xf_val_ok_num(0.0);
}

/* ── length ───────────────────────────────────────────────── */
/* length(str)   → number of characters (bytes)
 * length(arr)   → element count
 * length(map)   → key count
 * length(set)   → member count
 * length(num)   → number of bytes in the double representation (always 8) */
static xf_Value cg_length(xf_Value *args, size_t argc) {
    NEED(1);
    xf_Value v = args[0];
    if (v.state != XF_STATE_OK) return v;
    switch (v.type) {
        case XF_TYPE_STR:
            return xf_val_ok_num(v.data.str ? (double)v.data.str->len : 0.0);
        case XF_TYPE_ARR:
            return xf_val_ok_num(v.data.arr ? (double)v.data.arr->len : 0.0);
        case XF_TYPE_MAP:
        case XF_TYPE_SET:
            return xf_val_ok_num(v.data.map ? (double)v.data.map->order_len : 0.0);
        case XF_TYPE_NUM:
            return xf_val_ok_num((double)sizeof(double));   /* always 8 */
        default:
            return xf_val_nav(XF_TYPE_NUM);
    }
}

static xf_module_t *build_generics(void) {
    xf_module_t *m = xf_module_new("core.generics");
    FN("join",     XF_TYPE_STR,  cg_join);
    FN("split",    XF_TYPE_ARR,  cg_split);
    FN("strip",    XF_TYPE_STR,  cg_strip);
    FN("contains", XF_TYPE_NUM,  cg_contains);
    FN("length",   XF_TYPE_NUM,  cg_length);
    return m;
}

/* ============================================================
 * Module construction + registration
 * ============================================================ */


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

void core_register(SymTable *st) {
    xf_module_t *math_m     = build_math();
    xf_module_t *str_m      = build_str();
    xf_module_t *os_m       = build_os();
    xf_module_t *generics_m = build_generics();

    /* build the top-level core module */
    xf_module_t *core_m = xf_module_new("core");
    xf_module_set(core_m, "math",     xf_val_ok_module(math_m));
    xf_module_set(core_m, "str",      xf_val_ok_module(str_m));
    xf_module_set(core_m, "os",       xf_val_ok_module(os_m));
    xf_module_set(core_m, "generics", xf_val_ok_module(generics_m));

    /* release local refs — core_m retains them internally */
    xf_module_release(math_m);
    xf_module_release(str_m);
    xf_module_release(os_m);
    xf_module_release(generics_m);

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