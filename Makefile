# Makefile for the adaC compiler
#   make            -> builds ./adac (or adac.exe with MinGW)
#   make test       -> runs the test suite (needs python3 + unicorn)
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
SRC     := src/util.c src/lexer.c src/types.c src/parser.c src/sema.c src/x64.c src/pe.c src/codegen.c src/main.c
GEN     := src/prelude_data.c
OBJ     := $(SRC:.c=.o) $(GEN:.c=.o)
EXE     := adac$(if $(findstring Windows,$(OS)),.exe,)
EMBED   := tools/embed$(if $(findstring Windows,$(OS)),.exe,)

all: $(EXE)

$(EXE): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

$(EMBED): tools/embed.c
	$(CC) -O1 -o $@ $<

$(GEN): $(EMBED) runtime/core.adac runtime/gc.adac runtime/manual.adac
	./$(EMBED) ADAC_RT_CORE runtime/core.adac ADAC_RT_GC runtime/gc.adac ADAC_RT_MANUAL runtime/manual.adac > $@

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(EXE)
	python3 tests/run_tests.py

clean:
	rm -f src/*.o $(GEN) $(EXE) $(EMBED) examples/*.exe tests/out/*.exe

.PHONY: all test clean
