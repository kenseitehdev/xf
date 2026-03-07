#include "../include/repl.h"
#include "../include/parser.h"
#include "../include/ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <ctype.h>

#define XF_VERSION "0.6.4"
#define PROMPT_NORMAL ">> "
#define PROMPT_CONT   ".. "

/* ============================================================
 * Terminal raw mode
 * ============================================================ */

static struct termios orig_termios;
static bool           raw_mode_active = false;

static void restore_termios(void) {
    if (raw_mode_active)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    raw_mode_active = false;
}

static bool enter_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) return false;
    atexit(restore_termios);

    struct termios raw = orig_termios;
    raw.c_iflag &= (unsigned)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (unsigned)~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= (unsigned)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) return false;
    raw_mode_active = true;
    return true;
}

static void exit_raw_mode(void) {
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_active = false;
    }
}

static int __attribute__((unused)) terminal_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}


/* ============================================================
 * Low-level display
 * ============================================================ */

static void write_str(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void write_ch(char c)         { write(STDOUT_FILENO, &c, 1); }

static void cursor_left(int n) {
    if (n <= 0) return;
    char buf[32]; snprintf(buf, sizeof(buf), "\x1b[%dD", n);
    write_str(buf);
}
static void __attribute__((unused)) cursor_right(int n) {
    if (n <= 0) return;
    char buf[32]; snprintf(buf, sizeof(buf), "\x1b[%dC", n);
    write_str(buf);
}

/* redraw the current line from scratch */
static void refresh_line(const LineEdit *ed, const char *prompt) {
    /* move to start of line */
    write_ch('\r');
    /* erase to end */
    write_str("\x1b[0K");
    /* prompt */
    write_str(prompt);
    /* buffer content */
    write(STDOUT_FILENO, ed->buf, ed->len);
    /* reposition cursor */
    size_t prompt_len = strlen(prompt);
    int total = (int)(prompt_len + ed->len);
    int pos   = (int)(prompt_len + ed->cur);
    if (pos < total) cursor_left(total - pos);
}


/* ============================================================
 * History
 * ============================================================ */

void history_init(History *h) {
    memset(h, 0, sizeof(*h));
    h->cap     = 64;
    h->entries = malloc(sizeof(char *) * h->cap);
    h->cursor  = -1;
}

void history_free(History *h) {
    for (size_t i = 0; i < h->count; i++) free(h->entries[i]);
    free(h->entries);
    memset(h, 0, sizeof(*h));
}

void history_push(History *h, const char *line) {
    if (!line || !*line) return;
    /* don't duplicate last entry */
    if (h->count > 0 && strcmp(h->entries[h->count-1], line) == 0) {
        h->cursor = -1;
        return;
    }
    /* cap */
    if (h->count >= REPL_HIST_MAX) {
        free(h->entries[0]);
        memmove(h->entries, h->entries+1, sizeof(char*)*(h->count-1));
        h->count--;
    }
    if (h->count >= h->cap) {
        h->cap *= 2;
        h->entries = realloc(h->entries, sizeof(char*)*h->cap);
    }
    h->entries[h->count++] = strdup(line);
    h->cursor = -1;
}

void history_load(History *h, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[REPL_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n > 0) history_push(h, line);
    }
    fclose(f);
}

void history_save(const History *h, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    /* save last REPL_HIST_MAX entries */
    size_t start = h->count > REPL_HIST_MAX ? h->count - REPL_HIST_MAX : 0;
    for (size_t i = start; i < h->count; i++)
        fprintf(f, "%s\n", h->entries[i]);
    fclose(f);
}

void history_reset_cursor(History *h) { h->cursor = -1; }

const char *history_prev(History *h) {
    if (h->count == 0) return NULL;
    if (h->cursor < 0) h->cursor = (int)h->count;
    if (h->cursor > 0) h->cursor--;
    return h->entries[h->cursor];
}

const char *history_next(History *h) {
    if (h->cursor < 0) return NULL;
    h->cursor++;
    if (h->cursor >= (int)h->count) { h->cursor = -1; return NULL; }
    return h->entries[h->cursor];
}


/* ============================================================
 * Line editor — insert / delete / move
 * ============================================================ */

static void ed_insert(LineEdit *ed, char c) {
    if (ed->len >= REPL_LINE_MAX-1) return;
    memmove(ed->buf + ed->cur + 1, ed->buf + ed->cur, ed->len - ed->cur);
    ed->buf[ed->cur++] = c;
    ed->buf[++ed->len] = '\0';
}

static void ed_delete_back(LineEdit *ed) {    /* backspace */
    if (ed->cur == 0) return;
    memmove(ed->buf + ed->cur-1, ed->buf + ed->cur, ed->len - ed->cur);
    ed->buf[--ed->len] = '\0';
    ed->cur--;
}

static void ed_delete_fwd(LineEdit *ed) {     /* Del */
    if (ed->cur >= ed->len) return;
    memmove(ed->buf + ed->cur, ed->buf + ed->cur+1, ed->len - ed->cur - 1);
    ed->buf[--ed->len] = '\0';
}

static void ed_kill_eol(LineEdit *ed) {       /* Ctrl-K */
    size_t kill = ed->len - ed->cur;
    memcpy(ed->kill_buf, ed->buf + ed->cur, kill);
    ed->kill_buf[kill] = '\0';
    ed->kill_len = kill;
    ed->buf[ed->len = ed->cur] = '\0';
}

static void ed_kill_bol(LineEdit *ed) {       /* Ctrl-U */
    memcpy(ed->kill_buf, ed->buf, ed->cur);
    ed->kill_buf[ed->cur] = '\0';
    ed->kill_len = ed->cur;
    memmove(ed->buf, ed->buf + ed->cur, ed->len - ed->cur);
    ed->len -= ed->cur;
    ed->buf[ed->len] = '\0';
    ed->cur = 0;
}

static void ed_kill_word(LineEdit *ed) {      /* Ctrl-W */
    size_t end = ed->cur;
    while (end > 0 && ed->buf[end-1] == ' ') end--;
    while (end > 0 && ed->buf[end-1] != ' ') end--;
    size_t kill = ed->cur - end;
    memcpy(ed->kill_buf, ed->buf + end, kill);
    ed->kill_buf[kill] = '\0';
    ed->kill_len = kill;
    memmove(ed->buf + end, ed->buf + ed->cur, ed->len - ed->cur);
    ed->len -= kill;
    ed->buf[ed->len] = '\0';
    ed->cur = end;
}

static void ed_yank(LineEdit *ed) {           /* Ctrl-Y */
    for (size_t i = 0; i < ed->kill_len; i++) ed_insert(ed, ed->kill_buf[i]);
}

static void ed_set(LineEdit *ed, const char *s) {
    size_t n = strlen(s);
    if (n >= REPL_LINE_MAX) n = REPL_LINE_MAX-1;
    memcpy(ed->buf, s, n);
    ed->buf[n] = '\0';
    ed->len = ed->cur = n;
}

static void ed_clear(LineEdit *ed) {
    ed->buf[0] = '\0';
    ed->len = ed->cur = 0;
}


/* ============================================================
 * Main readline implementation
 * ============================================================ */

const char *repl_readline(Repl *r, const char *prompt) {
    LineEdit *ed = &r->edit;
    ed_clear(ed);

    if (!r->raw_mode) {
        /* fallback: no tty, use fgets */
        printf("%s", prompt);
        fflush(stdout);
        if (!fgets(ed->buf, REPL_LINE_MAX, stdin)) return NULL;
        size_t n = strlen(ed->buf);
        while (n > 0 && (ed->buf[n-1]=='\n'||ed->buf[n-1]=='\r')) ed->buf[--n]='\0';
        ed->len = ed->cur = n;
        return n ? ed->buf : NULL;
    }

    write_str(prompt);

    for (;;) {
        unsigned char seq[4];
        ssize_t n = read(STDIN_FILENO, seq, 1);
        if (n <= 0) return NULL;

        unsigned char c = seq[0];

        /* ── Ctrl chars ────────────────────────────────── */
        if (c == '\r' || c == '\n') {
            write_str("\r\n");
            return ed->buf;
        }

        if (c == 4) {           /* Ctrl-D — EOF on empty line */
            if (ed->len == 0) { write_str("\r\n"); return NULL; }
            ed_delete_fwd(ed);
            goto redraw;
        }
        if (c == 3) {           /* Ctrl-C — cancel line */
            write_str("^C\r\n");
            ed_clear(ed);
            history_reset_cursor(&r->hist);
            write_str(prompt);
            continue;
        }
        if (c == 1) { ed->cur = 0; goto redraw; }          /* Ctrl-A home */
        if (c == 5) { ed->cur = ed->len; goto redraw; }    /* Ctrl-E end  */
        if (c == 11) { ed_kill_eol(ed); goto redraw; }     /* Ctrl-K      */
        if (c == 21) { ed_kill_bol(ed); goto redraw; }     /* Ctrl-U      */
        if (c == 23) { ed_kill_word(ed); goto redraw; }    /* Ctrl-W      */
        if (c == 25) { ed_yank(ed); goto redraw; }         /* Ctrl-Y      */
        if (c == 12) {                                       /* Ctrl-L clear */
            write_str("\x1b[2J\x1b[H");
            goto redraw;
        }
        if (c == 127 || c == 8) {   /* Backspace / Ctrl-H */
            ed_delete_back(ed);
            goto redraw;
        }

        /* ── Escape sequences (arrows, Home, End, Del) ── */
        if (c == 27) {
            unsigned char s2[2];
            if (read(STDIN_FILENO, s2, 1) <= 0) continue;
            if (s2[0] == '[') {
                unsigned char s3[4];
                if (read(STDIN_FILENO, s3, 1) <= 0) continue;
                if (s3[0] >= '0' && s3[0] <= '9') {
                    /* extended: ESC [ n ~ or ESC [ 1 ; mod C/D */
                    unsigned char s4;
                    read(STDIN_FILENO, &s4, 1);
                    if (s4 == '~') {
                        if (s3[0] == '3') { ed_delete_fwd(ed); goto redraw; }
                        if (s3[0] == '1' || s3[0] == '7') { ed->cur = 0; goto redraw; }
                        if (s3[0] == '4' || s3[0] == '8') { ed->cur = ed->len; goto redraw; }
                    } else if (s4 == ';' && s3[0] == '1') {
                        /* ESC [ 1 ; mod dir — modifier + arrow */
                        unsigned char mod, dir;
                        read(STDIN_FILENO, &mod, 1);
                        read(STDIN_FILENO, &dir, 1);
                        if (mod == '5' || mod == '2') {
                            if (dir == 'C') {   /* Ctrl/Shift-Right: word forward */
                                while (ed->cur < ed->len && ed->buf[ed->cur] == ' ')  ed->cur++;
                                while (ed->cur < ed->len && ed->buf[ed->cur] != ' ')  ed->cur++;
                                goto redraw;
                            }
                            if (dir == 'D') {   /* Ctrl/Shift-Left: word backward */
                                while (ed->cur > 0 && ed->buf[ed->cur-1] == ' ')  ed->cur--;
                                while (ed->cur > 0 && ed->buf[ed->cur-1] != ' ')  ed->cur--;
                                goto redraw;
                            }
                        }
                    }
                } else {
                    switch (s3[0]) {
                    case 'A': {   /* Up — history prev */
                        const char *prev = history_prev(&r->hist);
                        if (prev) ed_set(ed, prev);
                        goto redraw;
                    }
                    case 'B': {   /* Down — history next */
                        const char *next = history_next(&r->hist);
                        if (next) ed_set(ed, next);
                        else      ed_clear(ed);
                        goto redraw;
                    }
                    case 'C':   /* Right */
                        if (ed->cur < ed->len) ed->cur++;
                        goto redraw;
                    case 'D':   /* Left */
                        if (ed->cur > 0) ed->cur--;
                        goto redraw;
                    case 'H':   /* Home (xterm) */
                        ed->cur = 0; goto redraw;
                    case 'F':   /* End  (xterm) */
                        ed->cur = ed->len; goto redraw;
                    }
                }
            } else if (s2[0] == 'O') {
                /* ESC O sequences */
                unsigned char s3[2];
                read(STDIN_FILENO, s3, 1);
                if (s3[0] == 'H') { ed->cur = 0; goto redraw; }
                if (s3[0] == 'F') { ed->cur = ed->len; goto redraw; }
            }
            continue;
        }

        /* ── printable ──────────────────────────────────── */
        if (c >= 32 && c < 127) {
            /* reset history cursor on new input */
            if (r->hist.cursor >= 0) history_reset_cursor(&r->hist);
            ed_insert(ed, (char)c);
        }

    redraw:
        refresh_line(ed, prompt);
    }
}


/* ============================================================
 * REPL command handling
 * ============================================================ */

static bool handle_command(Repl *r, const char *cmd_line) {
    /* strip leading colon */
    if (cmd_line[0] == ':') cmd_line++;

    char cmd[64] = {0};
    char arg[REPL_LINE_MAX] = {0};
    sscanf(cmd_line, "%63s %4095[^\n]", cmd, arg);

    if (strcmp(cmd,"quit")==0 || strcmp(cmd,"q")==0 || strcmp(cmd,"exit")==0)
        return false;

    if (strcmp(cmd,"help")==0) {
        printf("  :help            this message\r\n");
        printf("  :state           show all variable bindings\r\n");
        printf("  :type <name>     show type and state of a variable\r\n");
        printf("  :load <file>     load and evaluate a .xf file\r\n");
        printf("  :reload          reload the last loaded file\r\n");
        printf("  :history         show input history\r\n");
        printf("  :disasm          show VM chunk disassembly\r\n");
        printf("  :clear           reset environment\r\n");
        printf("  :quit            exit\r\n");
        return true;
    }

    if (strcmp(cmd,"state")==0) {
        sym_print_all(r->syms);
        return true;
    }

    if (strcmp(cmd,"type")==0 && arg[0]) {
        Symbol *s = sym_lookup(r->syms, arg, strlen(arg));
        if (s) {
            printf("  %s : %s [%s]\n", arg,
                   XF_TYPE_NAMES[s->type], XF_STATE_NAMES[s->state]);
        } else {
            printf("  '%s' not found\n", arg);
        }
        return true;
    }

    if (strcmp(cmd,"history")==0) {
        History *h = &r->hist;
        size_t start = h->count > 20 ? h->count-20 : 0;
        for (size_t i = start; i < h->count; i++)
            printf("  %3zu  %s\n", i+1, h->entries[i]);
        return true;
    }

    if (strcmp(cmd,"clear")==0) {
        sym_free(r->syms);
        sym_init(r->syms);
        sym_register_builtins(r->syms);
        interp_free(r->interp);
        interp_init(r->interp, r->syms, r->vm);
        printf("  environment cleared\r\n");
        return true;
    }

    if (strcmp(cmd,"load")==0 && arg[0]) {
        free(r->last_load);
        r->last_load = strdup(arg);
        /* fall through to load — return true means keep running */
        FILE *f = fopen(arg, "r");
        if (!f) { printf("ERR cannot open '%s'\n", arg); return true; }
        fseek(f, 0, SEEK_END); long sz=ftell(f); rewind(f);
        char *buf = malloc((size_t)sz+1);
        size_t n = fread(buf, 1, (size_t)sz, f); buf[n]='\0'; fclose(f);
        Lexer lex;
        xf_lex_init(&lex, buf, n, XF_SRC_FILE, arg);
        xf_tokenize(&lex);
        Program *prog = parse(&lex, r->syms);
        if (prog) { interp_run_program(r->interp, prog); ast_program_free(prog); }
        xf_lex_free(&lex); free(buf);
        return true;
    }

    if (strcmp(cmd,"reload")==0) {
        if (!r->last_load) { printf("  no file loaded yet\r\n"); return true; }
        /* fake a :load call */
        char tmp[REPL_LINE_MAX];
        snprintf(tmp, sizeof(tmp), "load %s", r->last_load);
        return handle_command(r, tmp);
    }

    printf("  unknown command ':%s' — try :help\n", cmd);
    return true;
}


/* ============================================================
 * REPL main loop
 * ============================================================ */

void repl_init(Repl *r, Interp *interp, SymTable *syms, VM *vm) {
    memset(r, 0, sizeof(*r));
    r->interp = interp;
    r->syms   = syms;
    r->vm     = vm;
    history_init(&r->hist);

    /* load history from home dir */
    const char *home = getenv("HOME");
    if (home) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", home, REPL_HIST_FILE);
        history_load(&r->hist, path);
    }

    r->raw_mode = enter_raw_mode();
}

void repl_free(Repl *r) {
    /* save history */
    const char *home = getenv("HOME");
    if (home) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", home, REPL_HIST_FILE);
        history_save(&r->hist, path);
    }
    exit_raw_mode();
    history_free(&r->hist);
    free(r->last_load);
    memset(r, 0, sizeof(*r));
}

void repl_run(Repl *r) {
    write_str("xf " XF_VERSION "  (:help for commands, :quit to exit)\r\n");

    /* accumulate multi-line input */
    char   src_buf[65536];
    size_t src_len = 0;

    for (;;) {
        const char *prompt = src_len > 0 ? PROMPT_CONT : PROMPT_NORMAL;
        const char *line   = repl_readline(r, prompt);

        if (!line) {
            if (src_len == 0) { write_str("\r\n"); break; }
            /* EOF mid-expression — discard and exit */
            write_str("\r\n"); break;
        }

        /* blank line — if accumulating, try to parse what we have */
        if (!line[0] && src_len == 0) continue;

        /* REPL command */
        if (line[0] == ':' && src_len == 0) {
            if (!handle_command(r, line)) break;
            continue;
        }

        /* append to source buffer */
        size_t line_len = strlen(line);
        if (src_len + line_len + 2 < sizeof(src_buf)) {
            memcpy(src_buf + src_len, line, line_len);
            src_len += line_len;
            src_buf[src_len++] = '\n';
            src_buf[src_len]   = '\0';
        }

        /* check continuation: lex and probe bracket depth */
        Lexer probe;
        xf_lex_init(&probe, src_buf, src_len, XF_SRC_REPL, "<repl>");
        xf_tokenize(&probe);
        bool needs_more = xf_lex_is_continuation(&probe);
        xf_lex_free(&probe);

        if (needs_more) continue;

        /* record in history (trimmed) */
        char trimmed[REPL_LINE_MAX];
        strncpy(trimmed, src_buf, sizeof(trimmed)-1);
        trimmed[sizeof(trimmed)-1] = '\0';
        /* strip trailing newline for history */
        size_t tl = strlen(trimmed);
        while (tl > 0 && (trimmed[tl-1]=='\n'||trimmed[tl-1]=='\r')) trimmed[--tl]='\0';
        history_push(&r->hist, trimmed);
        history_reset_cursor(&r->hist);

        /* parse */
        Lexer lex;
        xf_lex_init(&lex, src_buf, src_len, XF_SRC_REPL, "<repl>");
        xf_tokenize(&lex);
        TopLevel *item = parse_repl_line(&lex, r->syms);
        xf_lex_free(&lex);

        if (item) {
            xf_Value result = interp_eval_top(r->interp, item);
            ast_top_free(item);

            /* print result for expression statements */
            if (result.state != XF_STATE_NULL && result.state != XF_STATE_VOID) {
                xf_value_repl_print(result);
            }
            fflush(stdout);

            /* reset interp transient flags */
            r->interp->returning = false;
            r->interp->nexting   = false;
            r->interp->exiting   = false;
            r->interp->had_error = false;
        }

        src_len = 0;
        src_buf[0] = '\0';
    }
}