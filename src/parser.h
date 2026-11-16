#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>

/*
 * Turns one line of input into a pipeline of commands.
 *
 * This file does no system calls (no getenv, open, fork, ...). Variables are
 * looked up through a callback, so the parser can be unit-tested with a
 * fixed set of variables and the shell can plug in getenv and $?.
 *
 *     ls -l "my dir" | grep x > out.txt &
 *
 * becomes one Pipeline with two Commands; the second one has an output
 * redirection, and background is set.
 */

typedef enum { REDIR_IN, REDIR_OUT, REDIR_APPEND } RedirKind;

typedef struct {
    RedirKind kind;
    char *path;
} Redir;

typedef struct {
    char **argv; /* NULL-terminated, ready for execvp */
    size_t argc;
    Redir *redirs; /* applied left to right */
    size_t nredirs;
} Command;

typedef struct {
    Command *cmds;
    size_t ncmds;
    int background; /* line ended with & */
} Pipeline;

/* Returns the value of a variable, or NULL if it is not set. The name "?"
 * is used for $?. */
typedef const char *(*var_lookup_fn)(const char *name, void *ctx);

enum { PARSE_OK = 0, PARSE_EMPTY = 1, PARSE_ERROR = -1 };

/*
 * Parses `line`. On PARSE_OK the caller owns *out and must call
 * pipeline_free. On PARSE_EMPTY (blank line) and PARSE_ERROR there is nothing
 * to free; on error a message is written to err (if errlen > 0).
 * Running out of memory is reported as a PARSE_ERROR too.
 */
int parse_line(const char *line, var_lookup_fn lookup, void *ctx,
               Pipeline *out, char *err, size_t errlen);

void pipeline_free(Pipeline *p);

#endif
