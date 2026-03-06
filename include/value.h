#ifndef XF_VALUE_H
#define XF_VALUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

/* ============================================================
 * xf runtime — value & state system
 * ============================================================
 *
 * Every value in xf carries two orthogonal axes:
 *   state  — lifecycle/validity (7 states, quantum-inspired)
 *   type   — data kind (6 types + void)
 *
 * State is the primary axis. Type is secondary — meaningless
 * unless state is OK or VOID.
 *
 * State transitions are ONE-WAY and ATOMIC:
 *   UNDETERMINED → UNDEF → {OK, ERR, NAV, NULL, VOID}
 *   Terminal states never transition again.
 * ============================================================ */


/* ------------------------------------------------------------
 * States
 * ------------------------------------------------------------ */

#define XF_STATE_OK            0   /* value is valid and usable           */
#define XF_STATE_ERR           1   /* value carries a fault               */
#define XF_STATE_VOID          2   /* no return expected, value leaked     */
#define XF_STATE_NULL          3   /* no return expected, none given       */
#define XF_STATE_NAV           4   /* return expected, nothing returned    */
#define XF_STATE_UNDEF         5   /* exists, not yet resolved            */
#define XF_STATE_UNDETERMINED  6   /* not yet processed (pre-collapse)    */

#define XF_STATE_COUNT         7

/* is state terminal (no further collapse possible) */
#define XF_STATE_IS_TERMINAL(s) \
    ((s) == XF_STATE_OK   || \
     (s) == XF_STATE_ERR  || \
     (s) == XF_STATE_VOID || \
     (s) == XF_STATE_NULL || \
     (s) == XF_STATE_NAV)

/* is state an error condition */
#define XF_STATE_IS_ERROR(s) \
    ((s) == XF_STATE_ERR || (s) == XF_STATE_NAV)

/* is state unresolved */
#define XF_STATE_IS_PENDING(s) \
    ((s) == XF_STATE_UNDETERMINED || (s) == XF_STATE_UNDEF)

/* human-readable state names */
static const char *const XF_STATE_NAMES[XF_STATE_COUNT] = {
    "OK", "ERR", "VOID", "NULL", "NAV", "UNDEF", "UNDETERMINED"
};


/* ------------------------------------------------------------
 * Types
 * ------------------------------------------------------------ */

#define XF_TYPE_VOID   0   /* no type — used for void fn returns          */
#define XF_TYPE_NUM    1   /* double precision float                       */
#define XF_TYPE_STR    2   /* interned/ref-counted string                  */
#define XF_TYPE_MAP    3   /* associative array (hash map)                 */
#define XF_TYPE_SET    4   /* unique value collection                      */
#define XF_TYPE_ARR    5   /* ordered array                                */
#define XF_TYPE_FN     6   /* callable — first class                       */
#define XF_TYPE_REGEX  7   /* compiled regex — second class                */
#define XF_TYPE_MODULE 8   /* namespace module (core.math etc.)            */

#define XF_TYPE_COUNT  9

static const char *const XF_TYPE_NAMES[XF_TYPE_COUNT] = {
    "void", "num", "str", "map", "set", "arr", "fn", "regex", "module"
};


/* ------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------ */

typedef struct xf_value       xf_value_t;
typedef struct xf_str         xf_str_t;
typedef struct xf_map         xf_map_t;
typedef struct xf_set         xf_set_t;
typedef struct xf_arr         xf_arr_t;
typedef struct xf_fn          xf_fn_t;
typedef struct xf_regex       xf_regex_t;
typedef struct xf_err         xf_err_t;
typedef struct xf_param       xf_param_t;
typedef struct xf_env         xf_env_t;
typedef struct xf_atomic_value xf_atomic_value_t;
typedef struct xf_module      xf_module_t;

/* short aliases — use these in new code */
typedef xf_value_t         xf_Value;
typedef xf_str_t           xf_Str;
typedef xf_err_t           xf_Err;
typedef xf_fn_t            xf_Fn;
typedef xf_regex_t         xf_Regex;
typedef xf_atomic_value_t  xf_AtomicValue;


/* ------------------------------------------------------------
 * String — ref-counted, length-prefixed, UTF-8
 * ------------------------------------------------------------ */

struct xf_str {
    atomic_int  refcount;
    size_t      len;
    uint32_t    hash;       /* cached hash, 0 = not computed */
    char        data[];     /* flexible array, null-terminated */
};

xf_str_t *xf_str_new(const char *data, size_t len);
xf_str_t *xf_str_from_cstr(const char *cstr);
xf_str_t *xf_str_retain(xf_str_t *s);
void      xf_str_release(xf_str_t *s);
uint32_t  xf_str_hash(xf_str_t *s);
int       xf_str_cmp(const xf_str_t *a, const xf_str_t *b);


/* ------------------------------------------------------------
 * Function — carries full type contract
 * ------------------------------------------------------------ */

struct xf_param {
    xf_str_t   *name;
    uint8_t     type;        /* expected type                 */
    bool        has_default;
    xf_value_t *default_val; /* NULL if no default            */
};

struct xf_fn {
    atomic_int  refcount;
    xf_str_t   *name;
    uint8_t     return_type;  /* XF_TYPE_* — what caller expects  */
    xf_param_t *params;
    size_t      param_count;
    void       *body;         /* AST node — opaque at this layer  */
    xf_env_t   *closure;     /* captured environment             */
    bool        is_native;   /* C function vs xf function        */
    xf_value_t *(*native)(xf_value_t **args, size_t argc); /* legacy native */
    xf_value_t  (*native_v)(xf_value_t *args, size_t argc); /* value-return native */
};


/* ------------------------------------------------------------
 * Regex — second class (opaque, passable, not decomposable)
 * ------------------------------------------------------------ */

struct xf_regex {
    atomic_int  refcount;
    xf_str_t   *pattern;
    uint32_t    flags;       /* XF_RE_* flags */
    void       *compiled;   /* opaque compiled regex handle */
};

#define XF_RE_GLOBAL     (1 << 0)
#define XF_RE_ICASE      (1 << 1)
#define XF_RE_MULTILINE  (1 << 2)
#define XF_RE_EXTENDED   (1 << 3)


/* ------------------------------------------------------------
 * Error context — carried by ERR state values
 * ------------------------------------------------------------ */

struct xf_err {
    atomic_int   refcount;
    xf_str_t    *message;
    xf_str_t    *source;     /* filename or "repl"          */
    uint32_t     line;
    uint32_t     col;
    xf_err_t    *cause;      /* chained error (child ERR)   */
    xf_value_t **siblings;   /* other child results         */
    size_t       sibling_count;
};

xf_err_t *xf_err_new(const char *msg, const char *src, uint32_t line, uint32_t col);
xf_err_t *xf_err_retain(xf_err_t *e);
void      xf_err_release(xf_err_t *e);
xf_err_t *xf_err_chain(xf_err_t *parent, xf_err_t *cause,
                        xf_value_t **siblings, size_t sibling_count);


/* ------------------------------------------------------------
 * Map, Set, Array — structs defined AFTER struct xf_value below
 * (xf_map_slot_t embeds xf_value_t so must follow the full definition)
 * ------------------------------------------------------------ */

xf_arr_t *xf_arr_new(void);
xf_arr_t *xf_arr_retain(xf_arr_t *a);
void      xf_arr_release(xf_arr_t *a);
void       xf_arr_push(xf_arr_t *a, xf_value_t v);
xf_value_t xf_arr_pop(xf_arr_t *a);
void       xf_arr_unshift(xf_arr_t *a, xf_value_t v);
xf_value_t xf_arr_shift(xf_arr_t *a);
void       xf_arr_delete(xf_arr_t *a, size_t idx);
xf_value_t xf_arr_get(const xf_arr_t *a, size_t idx);
void       xf_arr_set(xf_arr_t *a, size_t idx, xf_value_t v);

xf_map_t  *xf_map_new(void);
xf_map_t  *xf_map_retain(xf_map_t *m);
void       xf_map_release(xf_map_t *m);
xf_value_t xf_map_get(const xf_map_t *m, const xf_str_t *key);
void       xf_map_set(xf_map_t *m, xf_str_t *key, xf_value_t val);
bool       xf_map_delete(xf_map_t *m, const xf_str_t *key);
size_t     xf_map_count(const xf_map_t *m);


/* ------------------------------------------------------------
 * Value — the universal runtime type
 *
 * state + type are the primary fields, always valid.
 * data union is only meaningful when state == OK or VOID.
 * err  is only meaningful when state == ERR.
 * ------------------------------------------------------------ */

struct xf_value {
    uint8_t  state;    /* XF_STATE_* */
    uint8_t  type;     /* XF_TYPE_*  */

    union {
        double       num;
        xf_str_t    *str;
        xf_map_t    *map;
        xf_set_t    *set;
        xf_arr_t    *arr;
        xf_fn_t     *fn;
        xf_regex_t  *re;
        xf_module_t *mod;
    } data;

    xf_err_t *err;    /* non-NULL only when state == XF_STATE_ERR */
};


/* ------------------------------------------------------------
 * arr / map / set / module structs
 * (defined here because they embed xf_value_t by value)
 * ------------------------------------------------------------ */

/* ── arr ──────────────────────────────────────────────────── */
struct xf_arr {
    atomic_int  refcount;
    xf_value_t *items;
    size_t      len;
    size_t      cap;
};

/* ── map ──────────────────────────────────────────────────── */
typedef struct {
    xf_str_t   *key;    /* NULL = empty slot */
    xf_value_t  val;
} xf_map_slot_t;

struct xf_map {
    atomic_int     refcount;
    xf_map_slot_t *slots;
    size_t         used;
    size_t         cap;        /* always a power of 2 */
    xf_str_t     **order;      /* insertion-order key list */
    size_t         order_len;
    size_t         order_cap;
};

/* ── set ──────────────────────────────────────────────────── */
struct xf_set {
    atomic_int  refcount;
    void       *impl;   /* TODO */
};

/* ── module ───────────────────────────────────────────────── */
typedef struct {
    const char *name;
    xf_value_t  val;
} xf_module_entry_t;

struct xf_module {
    atomic_int         refcount;
    const char        *name;
    xf_module_entry_t *entries;
    size_t             count;
    size_t             cap;
};

xf_module_t *xf_module_new(const char *name);
xf_module_t *xf_module_retain(xf_module_t *m);
void         xf_module_release(xf_module_t *m);
void         xf_module_set(xf_module_t *m, const char *name, xf_value_t val);
xf_value_t   xf_module_get(const xf_module_t *m, const char *name);
xf_value_t   xf_val_ok_module(xf_module_t *m);
xf_value_t   xf_val_native_fn(const char *name, uint8_t ret_type,
                               xf_value_t (*fn)(xf_value_t *args, size_t argc));


/* ------------------------------------------------------------
 * Value constructors
 * ------------------------------------------------------------ */

/* terminal state constructors */
xf_value_t xf_val_ok_num(double n);
xf_value_t xf_val_ok_str(xf_str_t *s);
xf_value_t xf_val_ok_map(xf_map_t *m);
xf_value_t xf_val_ok_set(xf_set_t *s);
xf_value_t xf_val_ok_arr(xf_arr_t *a);
xf_value_t xf_val_ok_fn(xf_fn_t *f);
xf_value_t xf_val_ok_re(xf_regex_t *r);

xf_value_t xf_val_err(xf_err_t *e, uint8_t type);
xf_value_t xf_val_nav(uint8_t expected_type);
xf_value_t xf_val_null(void);
xf_value_t xf_val_void(xf_value_t inner);

/* pending state constructors */
xf_value_t xf_val_undef(uint8_t type);
xf_value_t xf_val_undetermined(uint8_t type);

/* convenience */
#define XF_NULL  (xf_val_null())
#define XF_UNDEF (xf_val_undef(XF_TYPE_VOID))


/* ------------------------------------------------------------
 * Atomic collapse — thread-safe state transition
 *
 * Collapse is ONE-WAY. Once a value reaches a terminal state
 * it cannot be changed. Uses CAS to ensure only one thread
 * wins the collapse race.
 * ------------------------------------------------------------ */

typedef struct xf_atomic_value {
    _Atomic uint8_t  state;
    uint8_t          type;
    union {
        double      num;
        xf_str_t   *str;
        xf_map_t   *map;
        xf_set_t   *set;
        xf_arr_t   *arr;
        xf_fn_t    *fn;
        xf_regex_t *re;
    } data;
    xf_err_t *err;
} xf_atomic_value_t;

/* attempt to collapse — returns true if this thread won the race */
bool xf_collapse(xf_atomic_value_t *av, xf_value_t resolved);

/* read current state atomically */
uint8_t xf_atomic_state(const xf_atomic_value_t *av);

/* snapshot atomic value into regular value (safe after terminal) */
xf_value_t xf_snapshot(const xf_atomic_value_t *av);


/* ------------------------------------------------------------
 * Type coercion — only valid on OK state values
 * ------------------------------------------------------------ */

/* coerce to num — returns NAV if not coercible */
xf_value_t xf_coerce_num(xf_value_t v);

/* coerce to str — all types stringify */
xf_value_t xf_coerce_str(xf_value_t v);

/* check if coercion is possible */
bool xf_can_coerce(xf_value_t v, uint8_t target_type);


/* ------------------------------------------------------------
 * State propagation helpers
 *
 * When operating on values, state takes priority over type.
 * ERR/NAV propagate. UNDEF/UNDETERMINED block.
 * ------------------------------------------------------------ */

/* given two values, return the dominant state */
uint8_t xf_dominant_state(xf_value_t a, xf_value_t b);

/* propagate: if v is non-OK, return v unchanged. else run f(v) */
#define XF_PROPAGATE(v, expr)             \
    (XF_STATE_IS_TERMINAL((v).state) &&   \
     (v).state != XF_STATE_OK             \
        ? (v)                             \
        : (expr))

/* collect child results for ERR context */
xf_value_t xf_collect_err(xf_value_t *children, size_t n,
                           const char *src, uint32_t line);


/* ------------------------------------------------------------
 * Display / debug
 * ------------------------------------------------------------ */

/* print value with state annotation — used by REPL */
void xf_value_print(xf_value_t v);

/* format: "=> 3  [num, OK]" */
void xf_value_repl_print(xf_value_t v);

/* format err context block for REPL collapse */
void xf_err_print(xf_err_t *e);


#endif /* XF_VALUE_H */
