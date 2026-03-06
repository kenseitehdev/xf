#ifndef XF_REPL_H
#define XF_REPL_H

#include "value.h"
#include "lexer.h"
#include "symTable.h"
#include "interp.h"
#include <stddef.h>
#include <stdbool.h>

/* ============================================================
 * xf REPL
 *
 * Line editor features (no readline dependency):
 *   - Arrow left/right  — cursor movement
 *   - Arrow up/down     — history navigation
 *   - Home / End        — jump to line start/end
 *   - Ctrl-A / Ctrl-E   — same as Home/End
 *   - Ctrl-K            — kill to end of line
 *   - Ctrl-U            — kill to start of line
 *   - Ctrl-W            — kill previous word
 *   - Delete / Backspace
 *   - Ctrl-C            — cancel current line
 *   - Ctrl-D            — exit on empty line
 *
 * History:
 *   - Stored in memory during session
 *   - Persisted to ~/.xf_history on exit
 *   - Loaded from ~/.xf_history on startup
 *   - Up/down arrows navigate; editing a history line forks it
 * ============================================================ */

#define REPL_LINE_MAX    4096
#define REPL_HIST_MAX    500
#define REPL_HIST_FILE   ".xf_history"

/* ------------------------------------------------------------
 * History
 * ------------------------------------------------------------ */
typedef struct {
    char  **entries;
    size_t  count;
    size_t  cap;
    int     cursor;     /* current position when navigating   */
} History;

void history_init(History *h);
void history_free(History *h);
void history_push(History *h, const char *line);
void history_load(History *h, const char *path);
void history_save(const History *h, const char *path);

/* Navigate: +1 = older, -1 = newer. Returns entry or NULL */
const char *history_prev(History *h);
const char *history_next(History *h);
void        history_reset_cursor(History *h);

/* ------------------------------------------------------------
 * Line editor state
 * ------------------------------------------------------------ */
typedef struct {
    char    buf[REPL_LINE_MAX];  /* line buffer                */
    size_t  len;                  /* chars in buffer            */
    size_t  cur;                  /* cursor position (0..len)   */
    char    kill_buf[REPL_LINE_MAX]; /* Ctrl-K/U/W clipboard   */
    size_t  kill_len;
} LineEdit;

/* ------------------------------------------------------------
 * REPL
 * ------------------------------------------------------------ */
typedef struct {
    History  hist;
    LineEdit edit;
    Interp  *interp;
    SymTable *syms;
    VM       *vm;
    char     *last_load;  /* last :load path for :reload      */
    bool      raw_mode;   /* is terminal in raw mode?         */
} Repl;

void repl_init(Repl *r, Interp *interp, SymTable *syms, VM *vm);
void repl_free(Repl *r);

/* run interactive loop until :quit or Ctrl-D */
void repl_run(Repl *r);

/* read one line with full line-editing (blocking).
 * Returns NULL on EOF / Ctrl-D on empty line.
 * Caller must NOT free the returned pointer (valid until next call). */
const char *repl_readline(Repl *r, const char *prompt);

#endif /* XF_REPL_H */
