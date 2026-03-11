#if defined(__linux__) || defined(__CYGWIN__)
#  define _GNU_SOURCE
#endif

#include "../public/xf.h"

#include "../include/value.h"
#include "../include/lexer.h"
#include "../include/ast.h"
#include "../include/symTable.h"
#include "../include/parser.h"
#include "../include/vm.h"
#include "../include/interp.h"
#include "../include/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

struct xf_State {
    SymTable syms;
    VM       vm;
    Interp   interp;

    Program *prog;
    Lexer    lex;
    char    *loaded_src;

    int       last_rc;
    xf_Format in_fmt;
    xf_Format out_fmt;
};

/* ------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------ */

static void xf_state_clear_loaded(xf_State *xf) {
    if (!xf) return;

    if (xf->prog) {
        ast_program_free(xf->prog);
        xf->prog = NULL;
    }

    if (xf->lex.tokens) {
        xf_lex_free(&xf->lex);
        memset(&xf->lex, 0, sizeof(xf->lex));
    }

    free(xf->loaded_src);
    xf->loaded_src = NULL;

    xf->last_rc = 0;
    xf->interp.had_error = false;
    xf->interp.err_msg[0] = '\0';
}

static void apply_format(VM *vm, int in_mode, int out_mode) {
    if (in_mode >= 0 && out_mode < 0) out_mode = in_mode;
    if (out_mode >= 0 && in_mode < 0) in_mode  = out_mode;

    if (in_mode >= 0) {
        vm->rec.in_mode = (uint8_t)in_mode;
        switch (in_mode) {
            case XF_OUTFMT_CSV:
                strncpy(vm->rec.fs, ",", sizeof(vm->rec.fs) - 1);
                vm->rec.fs[sizeof(vm->rec.fs) - 1] = '\0';
                break;
            case XF_OUTFMT_TSV:
                strncpy(vm->rec.fs, "\t", sizeof(vm->rec.fs) - 1);
                vm->rec.fs[sizeof(vm->rec.fs) - 1] = '\0';
                break;
            default:
                break;
        }
    }

    if (out_mode >= 0) {
        vm->rec.out_mode = (uint8_t)out_mode;
        switch (out_mode) {
            case XF_OUTFMT_CSV:
                strncpy(vm->rec.ofs, ",", sizeof(vm->rec.ofs) - 1);
                vm->rec.ofs[sizeof(vm->rec.ofs) - 1] = '\0';
                break;
            case XF_OUTFMT_TSV:
                strncpy(vm->rec.ofs, "\t", sizeof(vm->rec.ofs) - 1);
                vm->rec.ofs[sizeof(vm->rec.ofs) - 1] = '\0';
                break;
            default:
                break;
        }
    }
}

static char *xf_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int xf_parse_and_run_loaded(xf_State *xf,
                                   const char *src,
                                   size_t len,
                                   SrcMode mode,
                                   const char *name) {
    if (!xf || !src) return 1;

    xf_state_clear_loaded(xf);

    xf->loaded_src = strdup(src);
    if (!xf->loaded_src) {
        xf->last_rc = 1;
        return 1;
    }

    xf_lex_init(&xf->lex, xf->loaded_src, len, mode, name ? name : "<string>");
    xf_tokenize(&xf->lex);

    xf->prog = parse(&xf->lex, &xf->syms);
    if (!xf->prog) {
        xf->last_rc = 1;
        return 1;
    }

    xf->last_rc = interp_run_program(&xf->interp, xf->prog);
    if (xf->last_rc) return xf->last_rc;
    if (xf->interp.had_error) return 1;
    return 0;
}

/* ------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------ */

xf_State *xf_newstate(void) {
    xf_State *xf = calloc(1, sizeof(*xf));
    if (!xf) return NULL;

    sym_init(&xf->syms);
    core_register(&xf->syms);
    sym_register_builtins(&xf->syms);

    vm_init(&xf->vm, 1);
    interp_init(&xf->interp, &xf->syms, &xf->vm);

    xf->prog     = NULL;
    xf->loaded_src = NULL;
    xf->last_rc  = 0;
    xf->in_fmt   = XF_FMT_UNSET;
    xf->out_fmt  = XF_FMT_UNSET;
    memset(&xf->lex, 0, sizeof(xf->lex));

    return xf;
}

void xf_close(xf_State *xf) {
    if (!xf) return;

    xf_state_clear_loaded(xf);
    interp_free(&xf->interp);
    vm_free(&xf->vm);
    sym_free(&xf->syms);
    free(xf);
}

int xf_set_format(xf_State *xf, xf_Format in_fmt, xf_Format out_fmt) {
    if (!xf) return 1;

    xf->in_fmt  = in_fmt;
    xf->out_fmt = out_fmt;
    apply_format(&xf->vm, (int)in_fmt, (int)out_fmt);
    return 0;
}

int xf_set_max_jobs(xf_State *xf, int max_jobs) {
    if (!xf) return 1;
    if (max_jobs < 1) max_jobs = 1;

    vm_free(&xf->vm);
    vm_init(&xf->vm, max_jobs);
    apply_format(&xf->vm, (int)xf->in_fmt, (int)xf->out_fmt);

    interp_free(&xf->interp);
    interp_init(&xf->interp, &xf->syms, &xf->vm);

    return 0;
}

int xf_load_string(xf_State *xf, const char *src, const char *name) {
    if (!xf || !src) return 1;
    return xf_parse_and_run_loaded(xf, src, strlen(src), XF_SRC_FILE,
                                   name ? name : "<string>");
}

int xf_load_file(xf_State *xf, const char *path) {
    if (!xf || !path) return 1;

    char *buf = xf_read_file(path);
    if (!buf) return 1;

    int rc = xf_parse_and_run_loaded(xf, buf, strlen(buf), XF_SRC_FILE, path);
    free(buf);
    return rc;
}

int xf_run_loaded(xf_State *xf) {
    if (!xf || !xf->prog) return 1;
    if (xf->last_rc) return xf->last_rc;
    return xf->interp.had_error ? 1 : 0;
}

int xf_run_string(xf_State *xf, const char *src, const char *name) {
    return xf_load_string(xf, src, name);
}

int xf_run_file(xf_State *xf, const char *path) {
    return xf_load_file(xf, path);
}

int xf_feed_line(xf_State *xf, const char *line) {
    if (!xf || !xf->prog || !line) return 1;

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;

    interp_feed_record(&xf->interp, xf->prog, line, len);
    return xf->interp.had_error ? 1 : 0;
}

int xf_feed_file(xf_State *xf, FILE *fp, const char *filename) {
    if (!xf || !xf->prog || !fp) return 1;

    xf->vm.rec.fnr = 0;
    if (filename && *filename) {
        strncpy(xf->vm.rec.current_file, filename,
                sizeof(xf->vm.rec.current_file) - 1);
        xf->vm.rec.current_file[sizeof(xf->vm.rec.current_file) - 1] = '\0';
    }

    char line[65536];
    while (!xf->interp.exiting && fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        interp_feed_record(&xf->interp, xf->prog, line, len);
        if (xf->interp.had_error) return 1;
    }

    return 0;
}

int xf_run_end(xf_State *xf) {
    if (!xf || !xf->prog) return 1;
    interp_run_end(&xf->interp, xf->prog);
    return xf->interp.had_error ? 1 : 0;
}

int xf_had_error(const xf_State *xf) {
    if (!xf) return 1;
    return xf->interp.had_error ? 1 : 0;
}

const char *xf_last_error(const xf_State *xf) {
    if (!xf) return "xf: null state";
    if (xf->interp.err_msg[0]) return xf->interp.err_msg;
    return NULL;
}

void xf_clear_error(xf_State *xf) {
    if (!xf) return;
    xf->interp.had_error = false;
    xf->interp.err_msg[0] = '\0';
}

const char *xf_version(void) {
    return XF_VERSION;
}