CC ?= cc
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -O2 -g

.PHONY: all unit valgrind clean

all: test_parser

# The parser tests only need parser.c, which makes no system calls.
test_parser: tests/test_parser.c src/parser.c src/parser.h
	$(CC) $(CFLAGS) -o $@ tests/test_parser.c src/parser.c

unit: test_parser
	./test_parser

valgrind: test_parser
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./test_parser

clean:
	rm -f test_parser *.exe
