CC ?= cc
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -O2 -g
SAN_FLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -O1 -g \
	-fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-fno-omit-frame-pointer

SHELL_SRC = src/main.c src/parser.c src/exec.c src/builtins.c
HDR = src/parser.h src/shell.h

.PHONY: all test unit integration sanitize valgrind clean

all: msh

msh: $(SHELL_SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SHELL_SRC)

# The parser tests only need parser.c, which makes no system calls.
test_parser: tests/test_parser.c src/parser.c src/parser.h
	$(CC) $(CFLAGS) -o $@ tests/test_parser.c src/parser.c

# Sanitizer builds get their own names so they never meet Valgrind.
msh_san: $(SHELL_SRC) $(HDR)
	$(CC) $(SAN_FLAGS) -o $@ $(SHELL_SRC)

test_parser_san: tests/test_parser.c src/parser.c src/parser.h
	$(CC) $(SAN_FLAGS) -o $@ tests/test_parser.c src/parser.c

unit: test_parser
	./test_parser

integration: msh
	sh tests/integration.sh ./msh

test: unit integration

sanitize: test_parser_san msh_san
	./test_parser_san
	sh tests/integration.sh ./msh_san

valgrind: test_parser
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./test_parser

clean:
	rm -f msh msh_san test_parser test_parser_san *.exe
