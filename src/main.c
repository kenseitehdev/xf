#if defined(__linux__) || defined(__CYGWIN__)
#  define _GNU_SOURCE
#endif
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
#include "../lib/driver.h"

/* ============================================================
 * xf — runtime entry point
 *
 * Invocation modes:
 *   xf                          REPL
 *   xf -e 'expr'                inline execute
 *   xf -r script.xf             file execute
 *   xf -e 'expr' input.txt      inline with file input
 *   xf -r script.xf input.txt   file with input
 *
 * Format / transform:
 *   xf -f csv                   input+output as CSV
 *   xf -f tsv:json              TSV input, JSON output
 *   xf -t '$1 .. "," .. $3'     per-record transform (implicit print)
 *   xf -f csv -t 'toupper($1)'  format + transform pipeline
 *   xf -f csv:json data.csv     file conversion (no explicit program needed)
 * ============================================================ */

#define XF_VERSION "0.9.10"

/* ------------------------------------------------------------
 * Format helpers
 * ------------------------------------------------------------ */

/* Parse a format name → XF_OUTFMT_* constant (-1 on unknown) */
static int parse_fmt_name(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "text") == 0 || strcmp(s, "txt") == 0) return XF_OUTFMT_TEXT;
    if (strcmp(s, "csv")  == 0)                          return XF_OUTFMT_CSV;
    if (strcmp(s, "tsv")  == 0 || strcmp(s, "tab") == 0) return XF_OUTFMT_TSV;
    if (strcmp(s, "json") == 0)                          return XF_OUTFMT_JSON;
    return -1;
}

/* ------------------------------------------------------------
 * Source builder
 *
 * Collects all source fragments (from -r, one or more -e, one or
 * more -t) into a single heap-allocated source string, then parses
 * it once.  This ensures:
 *
 *   - BEGIN/END blocks and function declarations from -r/-e are
 *     visible to -t expressions.
 *   - Rules from -r/-e and from -t all coexist in the same Program
 *     and fire in declaration order per record.
 *   - BEGIN runs exactly once (not once per flag).
 *
 * Fragment ownership:
 *   - File content (from -r) is heap-allocated here; we free it.
 *   - -e strings are borrowed from argv; we do NOT free them.
 *   - -t generated script is heap-allocated here; we free it.
 * ------------------------------------------------------------ */

typedef struct {
    char  **parts;       /* array of string pointers  */
    bool   *owned;       /* true = we must free it    */
    size_t  count;
    size_t  cap;
} SrcBuilder;

static void sb_init(SrcBuilder *sb) {
    sb->cap   = 8;
    sb->parts = malloc(sizeof(char *) * sb->cap);
    sb->owned = malloc(sizeof(bool)   * sb->cap);
    sb->count = 0;
}

static void sb_add(SrcBuilder *sb, char *s, bool owned) {
    if (sb->count >= sb->cap) {
        sb->cap  *= 2;
        sb->parts = realloc(sb->parts, sizeof(char *) * sb->cap);
        sb->owned = realloc(sb->owned, sizeof(bool)   * sb->cap);
    }
    sb->parts[sb->count] = s;
    sb->owned[sb->count] = owned;
    sb->count++;
}

/* Join all parts into one heap-allocated string (with '\n' between each).
 * Caller must free the result. */
static char *sb_join(const SrcBuilder *sb) {
    if (sb->count == 0) return strdup("");

    size_t total = 1; /* NUL */
    for (size_t i = 0; i < sb->count; i++)
        total += strlen(sb->parts[i]) + 1; /* +1 for '\n' */

    char *out = malloc(total);
    char *p   = out;
    for (size_t i = 0; i < sb->count; i++) {
        size_t len = strlen(sb->parts[i]);
        memcpy(p, sb->parts[i], len);
        p += len;
        *p++ = '\n';
    }
    *p = '\0';
    return out;
}

static void sb_free(SrcBuilder *sb) {
    for (size_t i = 0; i < sb->count; i++)
        if (sb->owned[i]) free(sb->parts[i]);
    free(sb->parts);
    free(sb->owned);
    sb->count = 0;
}

/* Read a file into a heap-allocated buffer.  Returns NULL on error. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "xf: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "xf: out of memory reading '%s'\n", path);
        return NULL;
    }
    size_t n  = fread(buf, 1, (size_t)sz, f);
    buf[n]    = '\0';
    fclose(f);
    return buf;
}

/* Build a per-record rule from a -t expression:  { print (expr) } */
static char *make_transform_rule(const char *expr) {
    size_t len = strlen(expr);
    char  *buf = malloc(len + 16); /* "{ print (" + ") }\n\0" */
    if (!buf) return NULL;
    sprintf(buf, "{ print (%s) }\n", expr);
    return buf;
}

/* ------------------------------------------------------------
 * Config — parsed from argv
 * ------------------------------------------------------------ */

typedef struct {
    const char **inline_exprs;    /* -e arguments, in order     */
    size_t       inline_count;
    const char  *script_file;     /* -r argument                */
    const char **transforms;      /* -t arguments, in order     */
    size_t       transform_count;
    const char **input_files;     /* remaining positional args  */
    size_t       input_count;
    int          in_fmt;          /* XF_OUTFMT_* or -1 (unset)  */
    int          out_fmt;         /* XF_OUTFMT_* or -1 (unset)  */
    bool         no_implicit_print;
    bool         print_all;
    int          max_jobs;
    bool         strict;
    bool         lenient;
} Config;

static void config_init(Config *c) {
    memset(c, 0, sizeof(*c));
    c->max_jobs = 1;
    c->in_fmt   = -1;
    c->out_fmt  = -1;
}

static void config_free(Config *c) {
    free(c->inline_exprs);
    free(c->transforms);
    free(c->input_files);
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
        "  xf -r script.xf           execute script file\n"
        "  xf -e 'expr' file ...     inline with file input\n"
        "  xf -r script.xf file ...  script with file input\n"
        "\n"
        "flags:\n"
        "  -e expr   inline expression (may be repeated)\n"
        "  -r file   script file (.xf)\n"
        "  -f fmt    stream format (default: text)\n"
        "            single value applies to input AND output\n"
        "            use 'infmt:outfmt' to set separately (e.g. csv:json)\n"
        "  -t expr   per-record transform — result is printed (may repeat)\n"
        "            composes correctly with -e/-r (rules run together)\n"
        "            reads stdin when no input files given\n"
        "  -n        suppress implicit print\n"
        "  -p        print every record\n"
        "  -j N      parallel schedulables (default: 1)\n"
        "  -s        strict mode (NAV == ERR)\n"
        "  -l        lenient mode (NAV does not propagate)\n"
        "  -h        show this help\n"
        "  -v        show version\n"
        "\n"
        "format modes (-f):\n"
        "  text      whitespace-split fields, plain print  (default)\n"
        "  csv       RFC 4180 comma-separated with quoting\n"
        "  tsv       tab-separated (\\t delimiter)\n"
        "  json      JSON-object output per record\n"
        "\n"
        "examples:\n"
        "  xf -f csv -t 'toupper($1)'           uppercase first CSV column\n"
        "  xf -f csv:json data.csv              convert CSV to JSON\n"
        "  xf -f tsv -t '$1 .. \",\" .. $3'      reorder TSV columns\n"
        "  xf -e 'BEGIN{FS=\",\"}' -t '$2' f.txt  -e and -t compose correctly\n"
        "  xf -e '{ print NR, $0 }' file.txt   number every line\n"
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
    c->inline_exprs = malloc(sizeof(char *) * (size_t)(argc + 1));
    c->transforms   = malloc(sizeof(char *) * (size_t)(argc + 1));
    c->input_files  = malloc(sizeof(char *) * (size_t)(argc + 1));

    if (!c->inline_exprs || !c->transforms || !c->input_files) {
        fprintf(stderr, "xf: out of memory\n");
        return false;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            exit(0);
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("xf " XF_VERSION "\n");
            exit(0);
        }
        if (strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "xf: -e requires an argument\n");
                return false;
            }
            c->inline_exprs[c->inline_count++] = argv[i];
            continue;
        }
        if (strcmp(argv[i], "-r") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "xf: -r requires an argument\n");
                return false;
            }
            c->script_file = argv[i];
            continue;
        }

        /* -f [in:]out — stream format */
        if (strcmp(argv[i], "-f") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "xf: -f requires an argument\n");
                return false;
            }
            const char *fmtarg = argv[i];
            const char *colon  = strchr(fmtarg, ':');
            if (colon) {
                /* "infmt:outfmt" */
                size_t in_len = (size_t)(colon - fmtarg);
                char   in_buf[32] = {0};
                if (in_len < sizeof(in_buf)) memcpy(in_buf, fmtarg, in_len);
                c->in_fmt  = parse_fmt_name(in_buf);
                c->out_fmt = parse_fmt_name(colon + 1);
                if (c->in_fmt < 0) {
                    fprintf(stderr, "xf: unknown input format '%.*s' (valid: text,csv,tsv,json)\n",
                            (int)in_len, fmtarg);
                    return false;
                }
                if (c->out_fmt < 0) {
                    fprintf(stderr, "xf: unknown output format '%s' (valid: text,csv,tsv,json)\n",
                            colon + 1);
                    return false;
                }
            } else {
                int m = parse_fmt_name(fmtarg);
                if (m < 0) {
                    fprintf(stderr, "xf: unknown format '%s' (valid: text,csv,tsv,json)\n", fmtarg);
                    return false;
                }
                c->in_fmt = c->out_fmt = m;
            }
            continue;
        }

        /* -t expr — per-record transform */
        if (strcmp(argv[i], "-t") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "xf: -t requires an argument\n");
                return false;
            }
            c->transforms[c->transform_count++] = argv[i];
            continue;
        }

        if (strcmp(argv[i], "-n") == 0) { c->no_implicit_print = true; continue; }
        if (strcmp(argv[i], "-p") == 0) { c->print_all         = true; continue; }
        if (strcmp(argv[i], "-s") == 0) { c->strict            = true; continue; }
        if (strcmp(argv[i], "-l") == 0) { c->lenient           = true; continue; }
        if (strcmp(argv[i], "-j") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "xf: -j requires an argument\n");
                return false;
            }
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
 * main
 * ------------------------------------------------------------ */

int main(int argc, char **argv) {
    Config cfg;
    config_init(&cfg);

    if (!parse_args(argc, argv, &cfg)) {
        usage(stderr);
        config_free(&cfg);
        return 1;
    }

    /* init symbol table, VM and interpreter */
    SymTable syms;
    sym_init(&syms);
    core_register(&syms);
    sym_register_builtins(&syms);

    VM vm;
    vm_init(&vm, cfg.max_jobs);

    /* Apply -f format settings before any record processing */
    if (cfg.in_fmt >= 0 || cfg.out_fmt >= 0)
        xf_driver_apply_format(&vm, cfg.in_fmt, cfg.out_fmt);

    Interp it;
    interp_init(&it, &syms, &vm);

    int      rc   = 0;
    Program *prog = NULL;
    Lexer    lex;
    char    *combined_src = NULL;
    memset(&lex, 0, sizeof(lex));

    /* ── REPL mode ────────────────────────────────────────── */
    /* Skip REPL if -f is set with input files and no -n: that's a format
     * conversion (e.g. `xf -f csv:json data.csv`). */
    bool format_passthrough_mode =
        (cfg.in_fmt > 0 || cfg.out_fmt > 0) &&
        cfg.input_count > 0 &&
        !cfg.no_implicit_print;

    if (cfg.inline_count == 0 && !cfg.script_file && cfg.transform_count == 0
        && !format_passthrough_mode) {
        Repl repl;
        repl_init(&repl, &it, &syms, &vm);
        repl_run(&repl);
        repl_free(&repl);
        goto done;
    }

    /* ── Build combined source from all -r / -e / -t flags ── */
    bool passthrough = false;  /* true = feed_file prints records directly */

    {
        SrcBuilder sb;
        sb_init(&sb);

        if (cfg.script_file) {
            char *fbuf = read_file(cfg.script_file);
            if (!fbuf) {
                rc = 1;
                sb_free(&sb);
                goto done;
            }
            sb_add(&sb, fbuf, /*owned=*/true);
        }

        for (size_t i = 0; i < cfg.inline_count; i++)
            sb_add(&sb, (char *)cfg.inline_exprs[i], /*owned=*/false);

        for (size_t i = 0; i < cfg.transform_count; i++) {
            char *rule = make_transform_rule(cfg.transforms[i]);
            if (!rule) {
                fprintf(stderr, "xf: out of memory building transform\n");
                rc = 1;
                sb_free(&sb);
                goto done;
            }
            sb_add(&sb, rule, /*owned=*/true);
        }

        combined_src = sb_join(&sb);
        sb_free(&sb);
        if (!combined_src) {
            fprintf(stderr, "xf: out of memory building source\n");
            rc = 1;
            goto done;
        }

        const char *src_name = cfg.script_file ? cfg.script_file
                             : (cfg.transform_count > 0 ? "<transform>"
                                                        : "<inline>");

        /* If format_passthrough_mode with no user code, skip parsing entirely
         * and use the C-level passthrough path in feed_file. */
        bool format_active = (cfg.in_fmt > 0 || cfg.out_fmt > 0);
        bool has_user_code = (cfg.script_file || cfg.inline_count > 0
                              || cfg.transform_count > 0);

        if (format_active && cfg.input_count > 0
                          && !has_user_code && !cfg.no_implicit_print) {
            passthrough = true;
            /* Still parse empty source so we get an empty Program for END flow. */
        }

        rc = xf_driver_run_source_prog(combined_src, strlen(combined_src),
                                       XF_SRC_FILE, src_name,
                                       &it, &prog, &lex);
        if (rc) goto done;
    }

    bool has_rules = xf_driver_program_has_rules(prog);

    /* -p with no rules: passthrough via interp_print_record (-n suppresses) */
    if (cfg.print_all && !cfg.no_implicit_print && !has_rules)
        passthrough = true;

    /* ── Stream records ─────────────────────────────────────── */
    if (prog && (has_rules || passthrough)) {
        if (cfg.input_count > 0) {
            for (size_t fi = 0; fi < cfg.input_count && !it.exiting; fi++) {
                FILE *f = fopen(cfg.input_files[fi], "r");
                if (!f) {
                    fprintf(stderr, "xf: cannot open '%s'\n", cfg.input_files[fi]);
                    rc = 1;
                    goto done;
                }
                rc = xf_driver_feed_file(&it, prog, f, cfg.input_files[fi], passthrough);
                fclose(f);
                if (rc || it.had_error) goto done;
            }
        } else if (cfg.transform_count > 0 || passthrough) {
            /* -t with no files, or passthrough mode: read from stdin */
            rc = xf_driver_feed_file(&it, prog, stdin, "<stdin>", passthrough);
            if (rc || it.had_error) goto done;
        }
    }

    /* Run END blocks */
    if (prog) interp_run_end(&it, prog);

done:
    if (prog)       ast_program_free(prog);
    if (lex.tokens) xf_lex_free(&lex);
    free(combined_src);
    interp_free(&it);
    vm_free(&vm);
    sym_free(&syms);
    config_free(&cfg);
    return rc ? rc : (it.had_error ? 1 : 0);
}