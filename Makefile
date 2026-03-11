CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=
LDLIBS  ?= -lm -lpthread

OBJDIR  = obj
BINDIR  = bin
LIBDIR  = lib

BIN     = $(BINDIR)/xf
LIBXF   = $(LIBDIR)/static/libxf.a

RUNTIME_SRCS = \
	src/ast.c \
	src/core.c \
	src/interp.c \
	src/lexer.c \
	src/parser.c \
	src/symTable.c \
	src/value.c \
	src/vm.c \
	lib/driver.c \
	lib/api.c

CLI_SRCS = \
	src/main.c \
	src/repl.c

RUNTIME_OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(RUNTIME_SRCS))
CLI_OBJS     = $(patsubst %.c,$(OBJDIR)/%.o,$(CLI_SRCS))

all: $(LIBXF) $(BIN)

$(LIBXF): $(RUNTIME_OBJS)
	@mkdir -p $(LIBDIR)
	$(AR) rcs $@ $(RUNTIME_OBJS)

$(BIN): $(CLI_OBJS) $(LIBXF)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJS) $(LIBXF) $(LDFLAGS) $(LDLIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(BIN) $(LIBXF)

.PHONY: all clean