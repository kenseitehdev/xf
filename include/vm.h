#ifndef XF_VM_H
#define XF_VM_H

#include "value.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

/* ============================================================
 * xf VM — stack-based bytecode virtual machine
 *
 * Architecture:
 *   - Value stack (xf_Value[])
 *   - CallFrame stack — one per active invocation
 *   - Scheduler queue — each frame is a schedulable unit
 *   - Constant pool per Chunk
 *
 * Preemption happens at OP_CALL / OP_SPAWN / OP_YIELD.
 * The state system (UNDETERMINED → ... → OK/ERR) maps onto
 * the scheduler: UNDETERMINED=queued, UNDEF=running,
 * OK/NULL=done clean, ERR/NAV=done with fault.
 * ============================================================ */


/* ------------------------------------------------------------
 * Opcodes
 * ------------------------------------------------------------ */
typedef enum {
    /* stack */
    OP_PUSH_NUM,      /* [8 bytes f64]  push literal double      */
    OP_PUSH_STR,      /* [4 bytes u32]  push consts[idx] string  */
    OP_PUSH_TRUE,     /* push num 1.0                            */
    OP_PUSH_FALSE,    /* push num 0.0                            */
    OP_PUSH_NULL,     /* push NULL-state value                   */
    OP_PUSH_UNDEF,    /* push UNDEF-state value                  */
    OP_POP,
    OP_DUP,
    OP_SWAP,

    /* locals / globals */
    OP_LOAD_LOCAL,    /* [1 byte slot]                           */
    OP_STORE_LOCAL,   /* [1 byte slot]                           */
    OP_LOAD_GLOBAL,   /* [4 bytes u32 idx]                       */
    OP_STORE_GLOBAL,  /* [4 bytes u32 idx]                       */

    /* record variables */
    OP_LOAD_FIELD,    /* [1 byte n]   push $n                    */
    OP_STORE_FIELD,   /* [1 byte n]   pop → $n                   */
    OP_LOAD_NR,
    OP_LOAD_NF,
    OP_LOAD_FNR,
    OP_LOAD_FS,
    OP_LOAD_RS,
    OP_LOAD_OFS,
    OP_LOAD_ORS,

    /* arithmetic */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_NEG,

    /* comparison */
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE, OP_SPACESHIP,

    /* logical */
    OP_AND, OP_OR, OP_NOT,

    /* string */
    OP_CONCAT,

    /* regex */
    OP_MATCH,         /* pop re, str → bool                      */
    OP_NMATCH,

    /* state / type introspection */
    OP_GET_STATE,     /* pop v → push num (state as int)         */
    OP_GET_TYPE,      /* pop v → push num (type as int)          */
    OP_COALESCE,      /* pop b, a → a if OK, else b              */

    /* collections */
    OP_MAKE_ARR,      /* [2 bytes u16 n]  pop n → arr            */
    OP_MAKE_MAP,      /* [2 bytes u16 n]  pop n k/v pairs → map  */
    OP_MAKE_SET,      /* [2 bytes u16 n]  pop n → set            */
    OP_GET_IDX,       /* pop key, obj → obj[key]                 */
    OP_SET_IDX,       /* pop val, key, obj → (side-effect)       */
    OP_DELETE_IDX,    /* pop key, obj                            */

    /* calls */
    OP_CALL,          /* [1 byte argc]  pop fn, pop argc args    */
    OP_RETURN,        /* pop return value                        */
    OP_RETURN_NULL,   /* return NULL state                       */

    /* concurrency */
    OP_SPAWN,         /* [1 byte argc]  schedule, push handle id */
    OP_JOIN,          /* pop handle id, block until done         */
    OP_YIELD,         /* hint preemption                         */

    /* I/O */
    OP_PRINT,         /* [1 byte argc]  print n values to stdout */

    /* substitution */
    OP_SUBST,         /* [4 pat][4 rep][1 flags]                 */
    OP_TRANS,         /* [4 from][4 to]                          */

    /* control */
    OP_JUMP,          /* [2 bytes i16 offset]                    */
    OP_JUMP_IF,       /* [2 bytes i16] pop, jump if truthy       */
    OP_JUMP_NOT,      /* [2 bytes i16] pop, jump if falsy        */
    OP_JUMP_NAV,      /* [2 bytes i16] pop, jump if NAV/NULL     */

    OP_NEXT_RECORD,   /* advance stream to next record           */
    OP_EXIT,

    OP_NOP,
    OP_HALT,

    OP_COUNT
} OpCode;


/* ------------------------------------------------------------
 * Chunk — unit of bytecode
 * ------------------------------------------------------------ */
typedef struct {
    uint8_t  *code;
    size_t    len;
    size_t    cap;

    xf_Value *consts;     /* constant pool                       */
    size_t    const_len;
    size_t    const_cap;

    uint32_t *lines;      /* source line per byte                */
    size_t    line_len;

    const char *source;
} Chunk;

void     chunk_init(Chunk *c, const char *source);
void     chunk_free(Chunk *c);
void     chunk_write(Chunk *c, uint8_t byte, uint32_t line);
void     chunk_write_u16(Chunk *c, uint16_t v, uint32_t line);
void     chunk_write_u32(Chunk *c, uint32_t v, uint32_t line);
void     chunk_write_f64(Chunk *c, double v, uint32_t line);
uint32_t chunk_add_const(Chunk *c, xf_Value v);
uint32_t chunk_add_str_const(Chunk *c, const char *s, size_t len);
void     chunk_patch_jump(Chunk *c, size_t pos, int16_t offset);
void     chunk_disasm(const Chunk *c, const char *name);
size_t   chunk_disasm_instr(const Chunk *c, size_t off);


/* ------------------------------------------------------------
 * CallFrame
 * ------------------------------------------------------------ */
#define FRAME_LOCALS_MAX 256

typedef struct CallFrame {
    Chunk    *chunk;
    size_t    ip;
    xf_Value  locals[FRAME_LOCALS_MAX];
    size_t    local_count;
    xf_Value  return_val;
    uint8_t   expected_ret;      /* XF_TYPE_* from fn signature  */

    /* scheduler */
    uint8_t   sched_state;       /* XF_STATE_*                   */
    uint32_t  frame_id;          /* unique id for OP_JOIN        */
    struct CallFrame *caller;
} CallFrame;


/* ------------------------------------------------------------
 * Record context — current input line
 * ------------------------------------------------------------ */
#define FIELD_MAX 256

typedef struct {
    char     *buf;           /* owned copy of current record    */
    size_t    buf_len;
    char     *fields[FIELD_MAX];  /* pointers into buf           */
    size_t    field_count;
    uint64_t  nr;            /* NR  — global record number      */
    uint64_t  fnr;           /* FNR — per-file record number    */
    char      fs[64];        /* FS  — field separator           */
    char      rs[64];        /* RS  — record separator          */
    char      ofs[64];       /* OFS — output field separator    */
    char      ors[64];       /* ORS — output record separator   */
    char      ofmt[32];      /* OFMT — number format, default "%.6g" */
    uint8_t   out_mode;      /* XF_OUTFMT_* — current output mode   */
    uint8_t   in_mode;       /* XF_OUTFMT_* — current input mode    */
    char    **headers;       /* field name strings for JSON (owned) */
    size_t    header_count;
    bool      headers_set;   /* true once headers have been populated */

    /* ── implicit match/context variables ────────────────────────────── *
     * Populated after ~ / !~ matches and at record boundaries.         *
     * vm_set_match(), vm_set_err(), vm_set_file() manage these.        */
    char      current_file[512]; /* $file     — input filename         */
    xf_Value  last_match;        /* $match    — full matched text      */
    xf_Value  last_captures;     /* $captures — arr of group strings   */
    xf_Value  last_err;          /* $err      — last error message str */
} RecordCtx;


/* ------------------------------------------------------------
 * VM
 * ------------------------------------------------------------ */
#define VM_STACK_MAX   1024
#define VM_FRAMES_MAX   256
#define VM_SCHED_MAX    256

typedef enum { VM_OK, VM_ERR, VM_EXIT } VMResult;

typedef struct VM {
    /* value stack */
    xf_Value   stack[VM_STACK_MAX];
    size_t     stack_top;

    /* call frame stack */
    CallFrame  frames[VM_FRAMES_MAX];
    size_t     frame_count;

    /* globals (indexed by compiler-assigned slot) */
    xf_Value  *globals;
    size_t     global_count;
    size_t     global_cap;

    /* scheduler */
    CallFrame *sched[VM_SCHED_MAX];
    size_t     sched_count;
    uint32_t   next_id;
    int        max_jobs;

    /* print redirect file handle cache
     * Maps destination path → open FILE* so that successive print
     * statements to the same file don't open/close on every record.
     * Entries are flushed and closed by vm_redir_flush(). */
#define VM_REDIR_MAX 32
    struct {
        char  path[256];  /* destination path or command */
        FILE *fp;
        bool  is_pipe;
    } redir[VM_REDIR_MAX];
    size_t redir_count;

    /* record context */
    RecordCtx  rec;
    pthread_mutex_t rec_mu;    /* guards rec during split_record */

    /* pattern-action rule chunks (interp fills these) */
    Chunk    **rules;          /* compiled rule bodies           */
    xf_Value  *patterns;       /* compiled pattern (NULL=always) */
    size_t     rule_count;

    /* BEGIN / END */
    Chunk     *begin_chunk;
    Chunk     *end_chunk;

    /* error */
    bool       had_error;
    char       err_msg[512];
} VM;


/* ------------------------------------------------------------
 * VM API
 * ------------------------------------------------------------ */
void      vm_init(VM *vm, int max_jobs);
void      vm_free(VM *vm);

VMResult  vm_run_chunk(VM *vm, Chunk *chunk);
VMResult  vm_run_begin(VM *vm);
VMResult  vm_run_end(VM *vm);
VMResult  vm_feed_record(VM *vm, const char *rec, size_t len);
void      vm_split_record(VM *vm, const char *rec, size_t len);

void      vm_push(VM *vm, xf_Value v);
xf_Value  vm_pop(VM *vm);
xf_Value  vm_peek(const VM *vm, int dist);   /* 0 = top */

uint32_t  vm_alloc_global(VM *vm, xf_Value init);

void      vm_error(VM *vm, const char *fmt, ...);
void      vm_dump_stack(const VM *vm);

/* redirect file handle cache */
FILE     *vm_redir_open(VM *vm, const char *path, int op); /* op: 1=write,2=append,3=pipe */
void      vm_redir_flush(VM *vm);   /* flush+close all cached handles */

/* JSON header population: parse first record as column headers */
void      vm_capture_headers(VM *vm); /* call after split_record on NR==1 when JSON mode */

/* Record context snapshot — used by worker threads to get a private
 * copy of the current record without holding rec_mu during fn execution.
 * vm_rec_snapshot: deep-copies buf, re-pointers fields, retains xf_Values.
 * vm_rec_snapshot_free: releases owned memory (buf, headers, xf_Values).
 * Both are safe to call from any thread; snapshot takes rec_mu briefly. */
void      vm_rec_snapshot(VM *vm, RecordCtx *snap);
void      vm_rec_snapshot_free(RecordCtx *snap);

const char *opcode_name(OpCode op);

#endif /* XF_VM_H */