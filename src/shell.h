#ifndef SHELL_H
#define SHELL_H

#include <stddef.h>
#include <sys/types.h>

#include "parser.h"

/* A pipeline started with & that has not finished yet. */
typedef struct {
    int id;        /* the [1], [2] ... number shown to the user */
    pid_t *pids;   /* every process in the pipeline */
    size_t npids;
    size_t alive;  /* how many of pids have not been reaped yet */
    char *text;    /* the command line, for messages */
} Job;

typedef struct {
    int last_status; /* value of $? */
    int interactive; /* stdin is a terminal */
    int should_exit;
    int exit_code;
    Job *jobs;
    size_t njobs;
    char status_text[16]; /* $? as a string, for the parser */
} Shell;

/* exec.c */
int run_pipeline(Shell *sh, Pipeline *p, const char *line);
void reap_jobs(Shell *sh);
void free_jobs(Shell *sh);

/* builtins.c */
int is_builtin(const char *name);
int run_builtin(Shell *sh, Command *cmd); /* returns the exit status */

#endif
