/*
 * mini-shell: read a line, parse it, run it, repeat.
 *
 * Reads commands from stdin. With a terminal it shows a prompt; with a pipe
 * or a file (as in the integration tests) it just runs the lines.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "parser.h"
#include "shell.h"

/*
 * Ctrl-C: the terminal sends SIGINT to every process in the foreground
 * process group, which is the shell and whatever it is running. The child
 * has the default action and dies; the shell only needs to not die. The
 * handler does nothing; its job is to interrupt the blocking read or wait.
 */
static void on_sigint(int sig)
{
    (void)sig;
}

static const char *lookup_var(const char *name, void *ctx)
{
    Shell *sh = ctx;
    if (strcmp(name, "?") == 0) {
        snprintf(sh->status_text, sizeof sh->status_text, "%d", sh->last_status);
        return sh->status_text;
    }
    return getenv(name);
}

int main(void)
{
    Shell sh;
    memset(&sh, 0, sizeof sh);
    sh.interactive = isatty(STDIN_FILENO);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: we want getline/waitpid to return */
    sigaction(SIGINT, &sa, NULL);
    if (sh.interactive)
        signal(SIGQUIT, SIG_IGN);

    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        reap_jobs(&sh);
        if (sh.interactive) {
            printf("msh$ ");
            fflush(stdout);
        }

        errno = 0;
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            if (errno == EINTR) {
                /* Ctrl-C at the prompt: drop the line, start again */
                clearerr(stdin);
                if (sh.interactive)
                    printf("\n");
                sh.last_status = 130;
                continue;
            }
            break; /* end of input */
        }
        if (n > 0 && line[n - 1] == '\n')
            line[n - 1] = '\0';

        Pipeline p;
        char err[256];
        int r = parse_line(line, lookup_var, &sh, &p, err, sizeof err);
        if (r == PARSE_ERROR) {
            fprintf(stderr, "mini-shell: syntax error: %s\n", err);
            sh.last_status = 2;
            continue;
        }
        if (r == PARSE_EMPTY)
            continue;

        sh.last_status = run_pipeline(&sh, &p, line);
        pipeline_free(&p);
        if (sh.should_exit)
            break;
    }

    if (sh.interactive && !sh.should_exit)
        printf("\n");
    free(line);
    free_jobs(&sh);
    return sh.should_exit ? sh.exit_code : sh.last_status;
}
