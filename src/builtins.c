/*
 * Built-in commands. These have to run inside the shell process itself:
 * a child process cannot change the shell's working directory or
 * environment, and `exit` has to end the shell, not a child.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

extern char **environ;

static int valid_name(const char *s, size_t len)
{
    if (len == 0 || !(isalpha((unsigned char)s[0]) || s[0] == '_'))
        return 0;
    for (size_t i = 1; i < len; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_'))
            return 0;
    return 1;
}

static int builtin_cd(Command *cmd)
{
    if (cmd->argc > 2) {
        fprintf(stderr, "mini-shell: cd: too many arguments\n");
        return 1;
    }
    const char *dir = cmd->argc == 2 ? cmd->argv[1] : getenv("HOME");
    if (dir == NULL) {
        fprintf(stderr, "mini-shell: cd: HOME not set\n");
        return 1;
    }
    char old[4096];
    int have_old = getcwd(old, sizeof old) != NULL;
    if (chdir(dir) != 0) {
        fprintf(stderr, "mini-shell: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    /* keep $PWD and $OLDPWD in step, like other shells do */
    char now[4096];
    if (have_old)
        setenv("OLDPWD", old, 1);
    if (getcwd(now, sizeof now) != NULL)
        setenv("PWD", now, 1);
    return 0;
}

static int builtin_pwd(void)
{
    char buf[4096];
    if (getcwd(buf, sizeof buf) == NULL) {
        fprintf(stderr, "mini-shell: pwd: %s\n", strerror(errno));
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}

static int builtin_exit(Shell *sh, Command *cmd)
{
    if (cmd->argc > 2) {
        fprintf(stderr, "mini-shell: exit: too many arguments\n");
        return 1;
    }
    int code = sh->last_status;
    if (cmd->argc == 2) {
        char *end;
        errno = 0;
        long v = strtol(cmd->argv[1], &end, 10);
        if (errno != 0 || end == cmd->argv[1] || *end != '\0') {
            fprintf(stderr, "mini-shell: exit: %s: numeric argument required\n",
                    cmd->argv[1]);
            code = 2;
        } else {
            code = (int)(v & 0xff); /* exit statuses are 0-255 */
        }
    }
    sh->should_exit = 1;
    sh->exit_code = code;
    return code;
}

static int builtin_export(Command *cmd)
{
    if (cmd->argc == 1) {
        for (char **e = environ; *e != NULL; e++)
            printf("export %s\n", *e);
        return 0;
    }
    int status = 0;
    for (size_t i = 1; i < cmd->argc; i++) {
        const char *arg = cmd->argv[i];
        const char *eq = strchr(arg, '=');
        size_t name_len = eq != NULL ? (size_t)(eq - arg) : strlen(arg);
        if (!valid_name(arg, name_len)) {
            fprintf(stderr, "mini-shell: export: '%s': not a valid identifier\n", arg);
            status = 1;
            continue;
        }
        /* `export NAME` without a value: every variable here is already in
         * the environment, so there is nothing to do. */
        if (eq == NULL)
            continue;
        char *name = malloc(name_len + 1);
        if (name == NULL) {
            fprintf(stderr, "mini-shell: export: out of memory\n");
            return 1;
        }
        memcpy(name, arg, name_len);
        name[name_len] = '\0';
        if (setenv(name, eq + 1, 1) != 0) {
            fprintf(stderr, "mini-shell: export: %s\n", strerror(errno));
            status = 1;
        }
        free(name);
    }
    return status;
}

static int builtin_unset(Command *cmd)
{
    int status = 0;
    for (size_t i = 1; i < cmd->argc; i++) {
        if (!valid_name(cmd->argv[i], strlen(cmd->argv[i]))) {
            fprintf(stderr, "mini-shell: unset: '%s': not a valid identifier\n",
                    cmd->argv[i]);
            status = 1;
            continue;
        }
        unsetenv(cmd->argv[i]);
    }
    return status;
}

static int builtin_jobs(Shell *sh)
{
    for (size_t i = 0; i < sh->njobs; i++)
        printf("[%d] Running    %s\n", sh->jobs[i].id, sh->jobs[i].text);
    return 0;
}

static const char *const names[] = {"cd", "exit", "pwd", "export", "unset", "jobs"};

int is_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(name, names[i]) == 0)
            return 1;
    return 0;
}

int run_builtin(Shell *sh, Command *cmd)
{
    const char *name = cmd->argv[0];
    if (strcmp(name, "cd") == 0)
        return builtin_cd(cmd);
    if (strcmp(name, "pwd") == 0)
        return builtin_pwd();
    if (strcmp(name, "exit") == 0)
        return builtin_exit(sh, cmd);
    if (strcmp(name, "export") == 0)
        return builtin_export(cmd);
    if (strcmp(name, "unset") == 0)
        return builtin_unset(cmd);
    if (strcmp(name, "jobs") == 0)
        return builtin_jobs(sh);
    return 127;
}
