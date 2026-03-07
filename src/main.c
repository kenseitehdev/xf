#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
 
#include "../include/value.h"
#include "../include/lexer.h"
#include "../include/ast.h"
#include "../include/symTable.h"
#include "../include/parser.h"
#include "../include/vm.h"
#include "../include/interp.h"
#include "../include/repl.h"
#include "../include/core.h"
/* ============================================================
 * xf — runtime entry point
 *
 * Invocation modes:
 *   xf                        REPL
 *   xf -e 'expr'              inline execute
 *   xf -f script.xf           file execute
 *   xf -e 'expr' input.txt    inline with file input
 *   xf -f script.xf input.txt file with input
 * ============================================================ */

#define XF_VERSION "0.6.4"


/* ------------------------------------------------------------
 * Config — parsed from argv
 * ------------------------------------------------------------ */

typedef struct {
    const char **inline_exprs;   /* -e arguments, in order      */
    size_t       inline_count;
    const char  *script_file;    /* -f argument                 */
    const char **input_files;    /* remaining positional args   */
    size_t       input_count;
    bool         no_implicit_print;  /* -n                      */
    bool         print_all;          /* -p                      */
    int          max_jobs;           /* -j N                    */
    bool         strict;             /* -s                      */
    bool         lenient;            /* -l                      */
} Config;

static void config_init(Config *c) {
    memset(c, 0, sizeof(*c));
    c->max_jobs = 1;
}


/* ------------------------------------------------------------
 * Usage
 * ------------------------------------------------------------ */

static void usage(FILE *out) {
    fprintf(out,
        "xf " XF_VERSION " — modernized awk/perl stream processor\n"
        "\n"
        "usage:\n"
        "  xf                        start REPL\n"
        "  xf -e 'expr'              execute inline expression\n"
        "  xf -f script.xf           execute script file\n"
        "  xf -e 'expr' file ...     inline with file input\n"
        "  xf -f script.xf file ...  script with file input\n"
        "\n"
        "flags:\n"
        "  -e expr   inline expression (may be repeated)\n"
        "  -f file   script file (.xf)\n"
        "  -n        suppress implicit print\n"
        "  -p        print every record\n"
        "  -j N      parallel schedulables (default: 1)\n"
        "  -s        strict mode (NAV == ERR)\n"
        "  -l        lenient mode (NAV does not propagate)\n"
        "  -h        show this help\n"
        "  -v        show version\n"
        "\n"
        "REPL commands:\n"
        "  :help     show commands\n"
        "  :state    show all bindings\n"
        "  :type x   show type and state of x\n"
        "  :load f   load .xf file into session\n"
        "  :reload   reload last loaded file\n"
        "  :history  show input history\n"
        "  :disasm   show VM chunk disassembly\n"
        "  :clear    reset environment\n"
        "  :quit     exit\n"
    );
}


/* ------------------------------------------------------------
 * Argument parsing
 * ------------------------------------------------------------ */

static bool parse_args(int argc, char **argv, Config *c) {
    c->inline_exprs = malloc(sizeof(char *) * (size_t)argc);
    c->input_files  = malloc(sizeof(char *) * (size_t)argc);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout); exit(0);
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("xf " XF_VERSION "\n"); exit(0);
        }
        if (strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) { fprintf(stderr, "xf: -e requires an argument\n"); return false; }
            c->inline_exprs[c->inline_count++] = argv[i];
            continue;
        }
        if (strcmp(argv[i], "-f") == 0) {
            if (++i >= argc) { fprintf(stderr, "xf: -f requires an argument\n"); return false; }
            c->script_file = argv[i];
            continue;
        }
        if (strcmp(argv[i], "-n") == 0) { c->no_implicit_print = true; continue; }
        if (strcmp(argv[i], "-p") == 0) { c->print_all         = true; continue; }
        if (strcmp(argv[i], "-s") == 0) { c->strict            = true; continue; }
        if (strcmp(argv[i], "-l") == 0) { c->lenient           = true; continue; }
        if (strcmp(argv[i], "-j") == 0) {
            if (++i >= argc) { fprintf(stderr, "xf: -j requires an argument\n"); return false; }
            c->max_jobs = atoi(argv[i]);
            if (c->max_jobs < 1) c->max_jobs = 1;
            continue;
        }
        /* unknown flag */
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "xf: unknown flag '%s'\n", argv[i]);
            return false;
        }
        /* positional = input file */
        c->input_files[c->input_count++] = argv[i];
    }
    return true;
}


/* ------------------------------------------------------------
 * Run helpers
 * ------------------------------------------------------------ */

/* parse + run a source buffer; returns (rc, prog).
 * prog is returned still alive so the caller can feed records through it.
 * Caller must call ast_program_free(prog) and xf_lex_free(lex) themselves —
 * or use the simple run_source_free wrapper for programs without input files. */
static int run_source_prog(const char *src, size_t len,
                            SrcMode mode, const char *name,
                            Interp *it,
                            Program **out_prog, Lexer *lex_out) {
    xf_lex_init(lex_out, src, len, mode, name);
    xf_tokenize(lex_out);

    Program *prog = parse(lex_out, it->syms);
    if (!prog) { xf_lex_free(lex_out); *out_prog = NULL; return 1; }

    int rc = interp_run_program(it, prog);
    *out_prog = prog;
    return rc;
}

static int run_file_prog(const char *path, Interp *it,
                         Program **out_prog, Lexer *lex_out,
                         char **buf_out) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "xf: cannot open '%s'\n", path); return 1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    size_t n  = fread(buf, 1, (size_t)sz, f);
    buf[n]    = '\0';
    fclose(f);

    int rc = run_source_prog(buf, n, XF_SRC_FILE, path, it, out_prog, lex_out);
    *buf_out = buf;   /* caller owns the buffer */
    return rc;
}


/* ------------------------------------------------------------
 * main
 * ------------------------------------------------------------ */

int main(int argc, char **argv) {
    Config cfg;
    config_init(&cfg);

    if (!parse_args(argc, argv, &cfg)) {
        usage(stderr);
        return 1;
    }

    /* init symbol table, VM and interpreter */
    SymTable syms;
    sym_init(&syms);
    core_register(&syms);
    sym_register_builtins(&syms);

    VM vm;
    vm_init(&vm, cfg.max_jobs);

    Interp it;
    interp_init(&it, &syms, &vm);

    int       rc   = 0;
    Program  *prog = NULL;
    Lexer     lex;
    char     *sbuf = NULL;
    memset(&lex, 0, sizeof(lex));

    /* ── REPL mode ────────────────────────────────────────── */
    if (cfg.inline_count == 0 && !cfg.script_file) {
        Repl repl;
        repl_init(&repl, &it, &syms, &vm);
        repl_run(&repl);
        repl_free(&repl);
        goto done;
    }

    /* ── script file mode: -f ─────────────────────────────── */
    if (cfg.script_file) {
        rc = run_file_prog(cfg.script_file, &it, &prog, &lex, &sbuf);
        if (rc) goto done;
    }

    /* ── inline mode: -e (may be repeated) ───────────────── */
    for (size_t i = 0; i < cfg.inline_count; i++) {
        const char *src = cfg.inline_exprs[i];
        if (prog) { ast_program_free(prog); xf_lex_free(&lex); free(sbuf); prog = NULL; sbuf = NULL; }
        rc = run_source_prog(src, strlen(src), XF_SRC_INLINE, "<inline>",
                             &it, &prog, &lex);
        sbuf = NULL;   /* inline src is argv — not heap-owned */
        if (rc) goto done;
    }

    /* ── input files: stream records through pattern-action rules ── */
    if (prog && cfg.input_count > 0) {
        /* check whether the program has any rules */
        bool has_rules = false;
        for (size_t i = 0; i < prog->count; i++)
            if (prog->items[i]->kind == TOP_RULE) { has_rules = true; break; }

        if (has_rules) {
            for (size_t fi = 0; fi < cfg.input_count && !it.exiting; fi++) {
                FILE *f = fopen(cfg.input_files[fi], "r");
                if (!f) {
                    fprintf(stderr, "xf: cannot open '%s'\n", cfg.input_files[fi]);
                    rc = 1; goto done;
                }
                it.vm->rec.fnr = 0;   /* reset per-file record number */

                char line[65536];
                while (!it.exiting && fgets(line, sizeof(line), f)) {
                    size_t len = strlen(line);
                    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                        line[--len] = '\0';
                    interp_feed_record(&it, prog, line, len);
                    if (it.had_error) { fclose(f); goto done; }
                }
                fclose(f);
            }
        }
    }

    /* always run END — whether or not there were input files */
    if (prog) interp_run_end(&it, prog);

done:
    if (prog) ast_program_free(prog);
    if (lex.tokens) xf_lex_free(&lex);
    free(sbuf);
    interp_free(&it);
    vm_free(&vm);
    sym_free(&syms);
    free(cfg.inline_exprs);
    free(cfg.input_files);
    return rc;
}