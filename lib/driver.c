#if defined(__linux__) || defined(__CYGWIN__)
#  define _GNU_SOURCE
#endif

#include "driver.h"

#include <string.h>

void xf_driver_apply_format(VM *vm, int in_mode, int out_mode) {
    if (!vm) return;

    /* mirror when only one side is given */
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
                /* text: keep default whitespace splitting */
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

int xf_driver_run_source_prog(const char *src, size_t len,
                              SrcMode mode, const char *name,
                              Interp *it, Program **out_prog, Lexer *lex_out) {
    if (!src || !it || !out_prog || !lex_out) return 1;

    xf_lex_init(lex_out, src, len, mode, name);
    xf_tokenize(lex_out);

    Program *prog = parse(lex_out, it->syms);
    if (!prog) {
        xf_lex_free(lex_out);
        *out_prog = NULL;
        return 1;
    }

    int rc = interp_run_program(it, prog);
    *out_prog = prog;
    return rc;
}

int xf_driver_feed_file(Interp *it, Program *prog, FILE *f,
                        const char *filename, bool passthrough) {
    if (!it || !prog || !f) return 1;

    it->vm->rec.fnr = 0;
    if (filename && *filename) {
        strncpy(it->vm->rec.current_file, filename,
                sizeof(it->vm->rec.current_file) - 1);
        it->vm->rec.current_file[sizeof(it->vm->rec.current_file) - 1] = '\0';
    }

    char line[65536];
    while (!it->exiting && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (passthrough) {
            vm_split_record(it->vm, line, len);
            interp_print_record(it);
        } else {
            interp_feed_record(it, prog, line, len);
        }

        if (it->had_error) return 1;
    }

    return 0;
}

bool xf_driver_program_has_rules(const Program *prog) {
    if (!prog) return false;

    for (size_t i = 0; i < prog->count; i++) {
        if (prog->items[i] && prog->items[i]->kind == TOP_RULE)
            return true;
    }
    return false;
}