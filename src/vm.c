#include "../include/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <assert.h>

/* ============================================================
 * Chunk
 * ============================================================ */

void chunk_init(Chunk *c, const char *source) {
    memset(c, 0, sizeof(*c));
    c->source    = source;
    c->cap       = 64;
    c->code      = malloc(c->cap);
    c->const_cap = 16;
    c->consts    = malloc(sizeof(xf_Value) * c->const_cap);
    c->lines     = malloc(sizeof(uint32_t) * c->cap);
}

void chunk_free(Chunk *c) {
    free(c->code);
    free(c->consts);
    free(c->lines);
    memset(c, 0, sizeof(*c));
}

static void chunk_grow(Chunk *c) {
    if (c->len >= c->cap) {
        c->cap   *= 2;
        c->code   = realloc(c->code,  c->cap);
        c->lines  = realloc(c->lines, sizeof(uint32_t) * c->cap);
    }
}

void chunk_write(Chunk *c, uint8_t byte, uint32_t line) {
    chunk_grow(c);
    c->code[c->len]  = byte;
    c->lines[c->len] = line;
    c->len++;
}

void chunk_write_u16(Chunk *c, uint16_t v, uint32_t line) {
    chunk_write(c, (v >> 8) & 0xff, line);
    chunk_write(c,  v       & 0xff, line);
}

void chunk_write_u32(Chunk *c, uint32_t v, uint32_t line) {
    chunk_write(c, (v >> 24) & 0xff, line);
    chunk_write(c, (v >> 16) & 0xff, line);
    chunk_write(c, (v >>  8) & 0xff, line);
    chunk_write(c,  v        & 0xff, line);
}

void chunk_write_f64(Chunk *c, double v, uint32_t line) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    for (int i = 7; i >= 0; i--)
        chunk_write(c, (bits >> (i * 8)) & 0xff, line);
}

uint32_t chunk_add_const(Chunk *c, xf_Value v) {
    if (c->const_len >= c->const_cap) {
        c->const_cap *= 2;
        c->consts = realloc(c->consts, sizeof(xf_Value) * c->const_cap);
    }
    c->consts[c->const_len] = v;
    return (uint32_t)c->const_len++;
}

uint32_t chunk_add_str_const(Chunk *c, const char *s, size_t len) {
    xf_Str *str = xf_str_new(s, len);
    xf_Value v  = xf_val_ok_str(str);
    xf_str_release(str);
    return chunk_add_const(c, v);
}

void chunk_patch_jump(Chunk *c, size_t pos, int16_t offset) {
    c->code[pos]   = (uint8_t)((offset >> 8) & 0xff);
    c->code[pos+1] = (uint8_t)( offset        & 0xff);
}

/* ── disassembler ─────────────────────────────────────────── */

const char *opcode_name(OpCode op) {
    switch (op) {
        case OP_PUSH_NUM:    return "PUSH_NUM";
        case OP_PUSH_STR:    return "PUSH_STR";
        case OP_PUSH_TRUE:   return "PUSH_TRUE";
        case OP_PUSH_FALSE:  return "PUSH_FALSE";
        case OP_PUSH_NULL:   return "PUSH_NULL";
        case OP_PUSH_UNDEF:  return "PUSH_UNDEF";
        case OP_POP:         return "POP";
        case OP_DUP:         return "DUP";
        case OP_SWAP:        return "SWAP";
        case OP_LOAD_LOCAL:  return "LOAD_LOCAL";
        case OP_STORE_LOCAL: return "STORE_LOCAL";
        case OP_LOAD_GLOBAL: return "LOAD_GLOBAL";
        case OP_STORE_GLOBAL:return "STORE_GLOBAL";
        case OP_LOAD_FIELD:  return "LOAD_FIELD";
        case OP_STORE_FIELD: return "STORE_FIELD";
        case OP_LOAD_NR:     return "LOAD_NR";
        case OP_LOAD_NF:     return "LOAD_NF";
        case OP_LOAD_FNR:    return "LOAD_FNR";
        case OP_LOAD_FS:     return "LOAD_FS";
        case OP_LOAD_RS:     return "LOAD_RS";
        case OP_LOAD_OFS:    return "LOAD_OFS";
        case OP_LOAD_ORS:    return "LOAD_ORS";
        case OP_ADD:         return "ADD";
        case OP_SUB:         return "SUB";
        case OP_MUL:         return "MUL";
        case OP_DIV:         return "DIV";
        case OP_MOD:         return "MOD";
        case OP_POW:         return "POW";
        case OP_NEG:         return "NEG";
        case OP_EQ:          return "EQ";
        case OP_NEQ:         return "NEQ";
        case OP_LT:          return "LT";
        case OP_GT:          return "GT";
        case OP_LTE:         return "LTE";
        case OP_GTE:         return "GTE";
        case OP_SPACESHIP:   return "SPACESHIP";
        case OP_AND:         return "AND";
        case OP_OR:          return "OR";
        case OP_NOT:         return "NOT";
        case OP_CONCAT:      return "CONCAT";
        case OP_MATCH:       return "MATCH";
        case OP_NMATCH:      return "NMATCH";
        case OP_GET_STATE:   return "GET_STATE";
        case OP_GET_TYPE:    return "GET_TYPE";
        case OP_COALESCE:    return "COALESCE";
        case OP_MAKE_ARR:    return "MAKE_ARR";
        case OP_MAKE_MAP:    return "MAKE_MAP";
        case OP_MAKE_SET:    return "MAKE_SET";
        case OP_GET_IDX:     return "GET_IDX";
        case OP_SET_IDX:     return "SET_IDX";
        case OP_DELETE_IDX:  return "DELETE_IDX";
        case OP_CALL:        return "CALL";
        case OP_RETURN:      return "RETURN";
        case OP_RETURN_NULL: return "RETURN_NULL";
        case OP_SPAWN:       return "SPAWN";
        case OP_JOIN:        return "JOIN";
        case OP_YIELD:       return "YIELD";
        case OP_PRINT:       return "PRINT";
        case OP_SUBST:       return "SUBST";
        case OP_TRANS:       return "TRANS";
        case OP_JUMP:        return "JUMP";
        case OP_JUMP_IF:     return "JUMP_IF";
        case OP_JUMP_NOT:    return "JUMP_NOT";
        case OP_JUMP_NAV:    return "JUMP_NAV";
        case OP_NEXT_RECORD: return "NEXT_RECORD";
        case OP_EXIT:        return "EXIT";
        case OP_NOP:         return "NOP";
        case OP_HALT:        return "HALT";
        default:             return "?";
    }
}

static uint16_t read_u16(const Chunk *c, size_t off) {
    return (uint16_t)((c->code[off] << 8) | c->code[off+1]);
}
static uint32_t read_u32(const Chunk *c, size_t off) {
    return ((uint32_t)c->code[off]   << 24) |
           ((uint32_t)c->code[off+1] << 16) |
           ((uint32_t)c->code[off+2] <<  8) |
            (uint32_t)c->code[off+3];
}
static double read_f64(const Chunk *c, size_t off) {
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) bits = (bits << 8) | c->code[off+i];
    double v; memcpy(&v, &bits, 8); return v;
}

size_t chunk_disasm_instr(const Chunk *c, size_t off) {
    printf("%04zu  ", off);
    if (off > 0 && c->lines[off] == c->lines[off-1])
        printf("   | ");
    else
        printf("%4u ", c->lines[off]);

    OpCode op = (OpCode)c->code[off++];
    printf("%-16s", opcode_name(op));

    switch (op) {
        case OP_PUSH_NUM: {
            double v = read_f64(c, off);
            printf("  %g", v);
            off += 8;
            break;
        }
        case OP_PUSH_STR: {
            uint32_t idx = read_u32(c, off);
            xf_Value v   = c->consts[idx];
            if (v.type == XF_TYPE_STR && v.data.str)
                printf("  \"%s\"", v.data.str->data);
            off += 4;
            break;
        }
        case OP_LOAD_LOCAL:
        case OP_STORE_LOCAL:
        case OP_LOAD_FIELD:
        case OP_STORE_FIELD:
        case OP_CALL:
        case OP_SPAWN:
        case OP_PRINT:
            printf("  %u", c->code[off++]);
            break;
        case OP_LOAD_GLOBAL:
        case OP_STORE_GLOBAL:
            printf("  %u", read_u32(c, off));
            off += 4;
            break;
        case OP_JUMP:
        case OP_JUMP_IF:
        case OP_JUMP_NOT:
        case OP_JUMP_NAV: {
            int16_t delta = (int16_t)read_u16(c, off);
            printf("  %+d  (→ %zu)", delta, off + 2 + (size_t)delta);
            off += 2;
            break;
        }
        case OP_MAKE_ARR:
        case OP_MAKE_MAP:
        case OP_MAKE_SET:
            printf("  %u", read_u16(c, off));
            off += 2;
            break;
        default: break;
    }
    printf("\n");
    return off;
}

void chunk_disasm(const Chunk *c, const char *name) {
    printf("== %s ==\n", name);
    for (size_t off = 0; off < c->len; )
        off = chunk_disasm_instr(c, off);
}


/* ============================================================
 * VM — init / free
 * ============================================================ */

void vm_init(VM *vm, int max_jobs) {
    memset(vm, 0, sizeof(*vm));
    vm->max_jobs   = max_jobs > 0 ? max_jobs : 1;
    vm->global_cap = 64;
    vm->globals    = malloc(sizeof(xf_Value) * vm->global_cap);

    /* default record separators */
    strcpy(vm->rec.fs,  " ");
    strcpy(vm->rec.rs,  "\n");
    strcpy(vm->rec.ofs,  " ");
    strcpy(vm->rec.ors,  "\n");
    strcpy(vm->rec.ofmt, "%.6g");
    vm->rec.out_mode     = 0;
    vm->rec.headers      = NULL;
    vm->rec.header_count = 0;
}

void vm_free(VM *vm) {
    free(vm->globals);
    free(vm->rec.buf);
    for (size_t i = 0; i < vm->rec.header_count; i++) free(vm->rec.headers[i]);
    free(vm->rec.headers);
    /* rule chunks freed by interp */
    free(vm->rules);
    free(vm->patterns);
    memset(vm, 0, sizeof(*vm));
}


/* ============================================================
 * Stack
 * ============================================================ */

void vm_push(VM *vm, xf_Value v) {
    if (vm->stack_top >= VM_STACK_MAX) {
        vm_error(vm, "stack overflow");
        return;
    }
    vm->stack[vm->stack_top++] = v;
}

xf_Value vm_pop(VM *vm) {
    if (vm->stack_top == 0) {
        vm_error(vm, "stack underflow");
        return xf_val_nav(XF_TYPE_VOID);
    }
    return vm->stack[--vm->stack_top];
}

xf_Value vm_peek(const VM *vm, int dist) {
    if ((size_t)dist >= vm->stack_top) return xf_val_nav(XF_TYPE_VOID);
    return vm->stack[vm->stack_top - 1 - dist];
}


/* ============================================================
 * Globals
 * ============================================================ */

uint32_t vm_alloc_global(VM *vm, xf_Value init) {
    if (vm->global_count >= vm->global_cap) {
        vm->global_cap *= 2;
        vm->globals = realloc(vm->globals, sizeof(xf_Value)*vm->global_cap);
    }
    vm->globals[vm->global_count] = init;
    return (uint32_t)vm->global_count++;
}


/* ============================================================
 * Error
 * ============================================================ */

void vm_error(VM *vm, const char *fmt, ...) {
    vm->had_error = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->err_msg, sizeof(vm->err_msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "ERR ─────────────────────────────────\n");
    fprintf(stderr, "  %s\n", vm->err_msg);
    if (vm->frame_count > 0) {
        CallFrame *f = &vm->frames[vm->frame_count-1];
        uint32_t line = f->ip < f->chunk->line_len ? f->chunk->lines[f->ip] : 0;
        fprintf(stderr, "  at %s:%u\n", f->chunk->source, line);
    }
    fprintf(stderr, "─────────────────────────────────\n");
}

void vm_dump_stack(const VM *vm) {
    printf("  stack [%zu]: ", vm->stack_top);
    for (size_t i = 0; i < vm->stack_top && i < 8; i++) {
        xf_value_repl_print(vm->stack[i]);
        printf(" ");
    }
    printf("\n");
}


/* ============================================================
 * Record splitting
 * ============================================================ */

static void split_record(VM *vm, const char *rec, size_t len) {
    free(vm->rec.buf);
    vm->rec.buf     = malloc(len + 1);
    memcpy(vm->rec.buf, rec, len);
    vm->rec.buf[len] = '\0';
    vm->rec.buf_len  = len;

    const char *fs = vm->rec.fs;
    size_t fs_len  = strlen(fs);
    size_t fc = 0;

    if (fs_len == 1 && fs[0] == ' ') {
        /* awk default: split on runs of whitespace */
        char *p = vm->rec.buf;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            vm->rec.fields[fc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
            if (fc >= FIELD_MAX-1) break;
        }
    } else {
        char *p = vm->rec.buf;
        vm->rec.fields[fc++] = p;
        while (*p && fc < FIELD_MAX-1) {
            if (strncmp(p, fs, fs_len) == 0) {
                *p = '\0';
                p += fs_len;
                vm->rec.fields[fc++] = p;
            } else {
                p++;
            }
        }
    }
    vm->rec.field_count = fc;
    vm->rec.nr++;
    vm->rec.fnr++;
}


/* ============================================================
 * Value arithmetic helpers
 * ============================================================ */

static bool is_truthy(xf_Value v) {
    if (v.state != XF_STATE_OK) return false;
    if (v.type == XF_TYPE_NUM)  return v.data.num != 0.0;
    if (v.type == XF_TYPE_STR)  return v.data.str && v.data.str->len > 0;
    return true;
}

static xf_Value val_num(double n) { return xf_val_ok_num(n); }

static xf_Value val_add(xf_Value a, xf_Value b) {
    a = xf_coerce_num(a); b = xf_coerce_num(b);
    if (a.state != XF_STATE_OK) return a;
    if (b.state != XF_STATE_OK) return b;
    return val_num(a.data.num + b.data.num);
}
static xf_Value val_sub(xf_Value a, xf_Value b) {
    a = xf_coerce_num(a); b = xf_coerce_num(b);
    if (a.state != XF_STATE_OK) return a;
    if (b.state != XF_STATE_OK) return b;
    return val_num(a.data.num - b.data.num);
}
static xf_Value val_mul(xf_Value a, xf_Value b) {
    a = xf_coerce_num(a); b = xf_coerce_num(b);
    if (a.state != XF_STATE_OK) return a;
    if (b.state != XF_STATE_OK) return b;
    return val_num(a.data.num * b.data.num);
}
static xf_Value val_div(VM *vm, xf_Value a, xf_Value b) {
    a = xf_coerce_num(a); b = xf_coerce_num(b);
    if (b.state == XF_STATE_OK && b.data.num == 0.0) {
        vm_error(vm, "division by zero");
        return xf_val_err(xf_err_new("division by zero", "<vm>", 0, 0), XF_TYPE_NUM);
    }
    if (a.state != XF_STATE_OK) return a;
    if (b.state != XF_STATE_OK) return b;
    return val_num(a.data.num / b.data.num);
}
static xf_Value val_mod(VM *vm, xf_Value a, xf_Value b) {
    a = xf_coerce_num(a); b = xf_coerce_num(b);
    if (b.state == XF_STATE_OK && b.data.num == 0.0) {
        vm_error(vm, "modulo by zero");
        return xf_val_err(xf_err_new("modulo by zero", "<vm>", 0, 0), XF_TYPE_NUM);
    }
    if (a.state != XF_STATE_OK) return a;
    if (b.state != XF_STATE_OK) return b;
    return val_num(fmod(a.data.num, b.data.num));
}
static xf_Value val_pow(xf_Value a, xf_Value b) {
    a = xf_coerce_num(a); b = xf_coerce_num(b);
    if (a.state != XF_STATE_OK) return a;
    if (b.state != XF_STATE_OK) return b;
    return val_num(pow(a.data.num, b.data.num));
}
static xf_Value val_concat(xf_Value a, xf_Value b) {
    xf_Value sa = xf_coerce_str(a);
    xf_Value sb = xf_coerce_str(b);
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
static int val_cmp(xf_Value a, xf_Value b) {
    /* numeric if both coerce cleanly, else string */
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


/* ============================================================
 * Core execution loop
 * ============================================================ */

#define READ_U8()  (frame->chunk->code[frame->ip++])
#define READ_U16() (frame->ip += 2, (uint16_t)((frame->chunk->code[frame->ip-2] << 8) | frame->chunk->code[frame->ip-1]))
#define READ_U32() (frame->ip += 4, \
    ((uint32_t)frame->chunk->code[frame->ip-4] << 24) | \
    ((uint32_t)frame->chunk->code[frame->ip-3] << 16) | \
    ((uint32_t)frame->chunk->code[frame->ip-2] <<  8) | \
     (uint32_t)frame->chunk->code[frame->ip-1])
#define READ_F64() (frame->ip += 8, ({ \
    uint64_t _b = 0; \
    for (int _i = 8; _i > 0; _i--) _b = (_b << 8) | frame->chunk->code[frame->ip - _i]; \
    double _v; memcpy(&_v, &_b, 8); _v; }))

VMResult vm_run_chunk(VM *vm, Chunk *chunk) {
    if (vm->frame_count >= VM_FRAMES_MAX) {
        vm_error(vm, "call stack overflow");
        return VM_ERR;
    }

    CallFrame *frame = &vm->frames[vm->frame_count++];
    frame->chunk       = chunk;
    frame->ip          = 0;
    frame->local_count = 0;
    frame->sched_state = XF_STATE_UNDEF;

    for (;;) {
        if (frame->ip >= frame->chunk->len) break;

        OpCode op = (OpCode)READ_U8();
        xf_Value a, b;

        switch (op) {

        case OP_NOP: break;
        case OP_HALT: goto done;

        case OP_PUSH_NUM: {
            uint64_t bits = 0;
            for (int i = 0; i < 8; i++)
                bits = (bits << 8) | frame->chunk->code[frame->ip++];
            double v; memcpy(&v, &bits, 8);
            vm_push(vm, val_num(v));
            break;
        }
        case OP_PUSH_STR: {
            uint32_t idx = READ_U32();
            vm_push(vm, frame->chunk->consts[idx]);
            break;
        }
        case OP_PUSH_TRUE:  vm_push(vm, val_num(1.0)); break;
        case OP_PUSH_FALSE: vm_push(vm, val_num(0.0)); break;
        case OP_PUSH_NULL:  vm_push(vm, xf_val_null()); break;
        case OP_PUSH_UNDEF: vm_push(vm, xf_val_undef(XF_TYPE_VOID)); break;

        case OP_POP:  vm_pop(vm); break;
        case OP_DUP:  vm_push(vm, vm_peek(vm, 0)); break;
        case OP_SWAP: {
            xf_Value top = vm_pop(vm), sec = vm_pop(vm);
            vm_push(vm, top); vm_push(vm, sec);
            break;
        }

        case OP_LOAD_LOCAL: {
            uint8_t slot = READ_U8();
            vm_push(vm, frame->locals[slot]);
            break;
        }
        case OP_STORE_LOCAL: {
            uint8_t slot = READ_U8();
            frame->locals[slot] = vm_peek(vm, 0);
            if (frame->local_count <= slot) frame->local_count = slot+1;
            break;
        }
        case OP_LOAD_GLOBAL: {
            uint32_t idx = READ_U32();
            vm_push(vm, idx < vm->global_count
                         ? vm->globals[idx]
                         : xf_val_undef(XF_TYPE_VOID));
            break;
        }
        case OP_STORE_GLOBAL: {
            uint32_t idx = READ_U32();
            if (idx < vm->global_count) vm->globals[idx] = vm_peek(vm, 0);
            break;
        }

        case OP_LOAD_FIELD: {
            uint8_t n = READ_U8();
            if (n == 0) {
                xf_Str *s = xf_str_new(vm->rec.buf ? vm->rec.buf : "", vm->rec.buf_len);
                xf_Value v = xf_val_ok_str(s); xf_str_release(s);
                vm_push(vm, v);
            } else if (n <= vm->rec.field_count) {
                xf_Str *s = xf_str_from_cstr(vm->rec.fields[n-1]);
                xf_Value v = xf_val_ok_str(s); xf_str_release(s);
                vm_push(vm, v);
            } else {
                vm_push(vm, xf_val_ok_str(xf_str_from_cstr("")));
            }
            break;
        }
        case OP_STORE_FIELD: {
            /* TODO: field assignment rebuilds $0 */
            uint8_t _n = READ_U8(); (void)_n;
            vm_pop(vm);
            break;
        }

        case OP_LOAD_NR:  vm_push(vm, val_num((double)vm->rec.nr));   break;
        case OP_LOAD_NF:  vm_push(vm, val_num((double)vm->rec.field_count)); break;
        case OP_LOAD_FNR: vm_push(vm, val_num((double)vm->rec.fnr));  break;
        case OP_LOAD_FS:  { xf_Str *s=xf_str_from_cstr(vm->rec.fs);  vm_push(vm,xf_val_ok_str(s));xf_str_release(s);break; }
        case OP_LOAD_RS:  { xf_Str *s=xf_str_from_cstr(vm->rec.rs);  vm_push(vm,xf_val_ok_str(s));xf_str_release(s);break; }
        case OP_LOAD_OFS: { xf_Str *s=xf_str_from_cstr(vm->rec.ofs); vm_push(vm,xf_val_ok_str(s));xf_str_release(s);break; }
        case OP_LOAD_ORS: { xf_Str *s=xf_str_from_cstr(vm->rec.ors); vm_push(vm,xf_val_ok_str(s));xf_str_release(s);break; }

        case OP_ADD: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_add(a,b)); break;
        case OP_SUB: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_sub(a,b)); break;
        case OP_MUL: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_mul(a,b)); break;
        case OP_DIV: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_div(vm,a,b)); break;
        case OP_MOD: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_mod(vm,a,b)); break;
        case OP_POW: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_pow(a,b)); break;
        case OP_NEG: a=vm_pop(vm); {xf_Value n=xf_coerce_num(a); vm_push(vm,n.state==XF_STATE_OK?val_num(-n.data.num):n);} break;

        case OP_EQ:  b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_num(val_cmp(a,b)==0?1:0)); break;
        case OP_NEQ: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_num(val_cmp(a,b)!=0?1:0)); break;
        case OP_LT:  b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_num(val_cmp(a,b)<0 ?1:0)); break;
        case OP_GT:  b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_num(val_cmp(a,b)>0 ?1:0)); break;
        case OP_LTE: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_num(val_cmp(a,b)<=0?1:0)); break;
        case OP_GTE: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_num(val_cmp(a,b)>=0?1:0)); break;
        case OP_SPACESHIP: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_num((double)val_cmp(a,b))); break;

        case OP_AND: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_num(is_truthy(a)&&is_truthy(b)?1:0)); break;
        case OP_OR:  b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_num(is_truthy(a)||is_truthy(b)?1:0)); break;
        case OP_NOT: a=vm_pop(vm); vm_push(vm,val_num(is_truthy(a)?0:1)); break;

        case OP_CONCAT: b=vm_pop(vm); a=vm_pop(vm); vm_push(vm,val_concat(a,b)); break;

        case OP_MATCH:
        case OP_NMATCH: {
            /* simple substring match for now — full regex in interp */
            b = vm_pop(vm); a = vm_pop(vm);
            xf_Value sa = xf_coerce_str(a), sb = xf_coerce_str(b);
            bool found = (sa.state==XF_STATE_OK && sb.state==XF_STATE_OK)
                         ? (strstr(sa.data.str->data, sb.data.str->data) != NULL)
                         : false;
            vm_push(vm, val_num((op==OP_MATCH) == found ? 1.0 : 0.0));
            break;
        }

        case OP_GET_STATE: a=vm_pop(vm); vm_push(vm,val_num((double)a.state)); break;
        case OP_GET_TYPE:  a=vm_pop(vm); vm_push(vm,val_num((double)a.type));  break;
        case OP_COALESCE:
            b=vm_pop(vm); a=vm_pop(vm);
            vm_push(vm, (a.state==XF_STATE_OK) ? a : b);
            break;

        case OP_PRINT: {
            uint8_t argc = READ_U8();
            /* args are on stack in reverse — collect then print */
            xf_Value args[64];
            for (int i = argc-1; i >= 0; i--)
                args[i] = vm_pop(vm);
            for (int i = 0; i < argc; i++) {
                if (i > 0) printf("%s", vm->rec.ofs);
                xf_Value sv = xf_coerce_str(args[i]);
                if (sv.state==XF_STATE_OK && sv.data.str)
                    printf("%s", sv.data.str->data);
                else
                    printf("%s", XF_STATE_NAMES[args[i].state]);
            }
            printf("%s", vm->rec.ors);
            break;
        }

        case OP_JUMP: {
            int16_t delta = (int16_t)READ_U16();
            frame->ip += (size_t)((int)delta);
            break;
        }
        case OP_JUMP_IF: {
            int16_t delta = (int16_t)READ_U16();
            a = vm_pop(vm);
            if (is_truthy(a)) frame->ip += (size_t)((int)delta);
            break;
        }
        case OP_JUMP_NOT: {
            int16_t delta = (int16_t)READ_U16();
            a = vm_pop(vm);
            if (!is_truthy(a)) frame->ip += (size_t)((int)delta);
            break;
        }
        case OP_JUMP_NAV: {
            int16_t delta = (int16_t)READ_U16();
            a = vm_pop(vm);
            if (a.state==XF_STATE_NAV || a.state==XF_STATE_NULL)
                frame->ip += (size_t)((int)delta);
            break;
        }

        case OP_RETURN:
            frame->return_val = vm_pop(vm);
            goto done;
        case OP_RETURN_NULL:
            frame->return_val = xf_val_null();
            goto done;

        case OP_EXIT:
            vm->frame_count--;
            return VM_EXIT;

        case OP_NEXT_RECORD:
            goto done;   /* signals interp to advance stream */

        case OP_YIELD: break;   /* preemption hint — no-op for now */

        case OP_CALL:
        case OP_SPAWN:
        case OP_JOIN:
        case OP_MAKE_ARR:
        case OP_MAKE_MAP:
        case OP_MAKE_SET:
        case OP_GET_IDX:
        case OP_SET_IDX:
        case OP_DELETE_IDX:
        case OP_SUBST:
        case OP_TRANS:
            /* stub — interp layer handles these */
            break;

        default:
            vm_error(vm, "unknown opcode %d at ip=%zu", op, frame->ip-1);
            goto err;
        }

        if (vm->had_error) goto err;
        continue;

    err:
        vm->frame_count--;
        return VM_ERR;
    }

done:
    vm->frame_count--;
    return vm->had_error ? VM_ERR : VM_OK;
}

VMResult vm_feed_record(VM *vm, const char *rec, size_t len) {
    split_record(vm, rec, len);
    for (size_t i = 0; i < vm->rule_count; i++) {
        /* pattern check */
        if (vm->patterns) {
            xf_Value pat = vm->patterns[i];
            if (pat.state == XF_STATE_OK) {
                /* TODO: full pattern eval — for now skip non-null patterns */
            }
        }
        VMResult r = vm_run_chunk(vm, vm->rules[i]);
        if (r == VM_EXIT) return VM_EXIT;
        if (r == VM_ERR)  return VM_ERR;
    }
    return VM_OK;
}

void vm_split_record(VM *vm, const char *rec, size_t len) {
    split_record(vm, rec, len);
}

VMResult vm_run_begin(VM *vm) {
    if (!vm->begin_chunk) return VM_OK;
    return vm_run_chunk(vm, vm->begin_chunk);
}

VMResult vm_run_end(VM *vm) {
    if (!vm->end_chunk) return VM_OK;
    return vm_run_chunk(vm, vm->end_chunk);
}
