#ifndef XF_DRIVER_H
#define XF_DRIVER_H

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

#include "../include/ast.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/interp.h"
#include "../include/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

void xf_driver_apply_format(VM *vm, int in_mode, int out_mode);

int xf_driver_run_source_prog(const char *src, size_t len,
                              SrcMode mode, const char *name,
                              Interp *it, Program **out_prog, Lexer *lex_out);

int xf_driver_feed_file(Interp *it, Program *prog, FILE *f,
                        const char *filename, bool passthrough);

bool xf_driver_program_has_rules(const Program *prog);

#ifdef __cplusplus
}
#endif

#endif /* XF_DRIVER_H */