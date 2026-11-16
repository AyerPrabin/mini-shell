/*
 * Unit tests for the parser. Plain C, no framework.
 *
 * Each case is a line of input and the expected result written in a small
 * text form, which keeps the table easy to read:
 *
 *   [echo] [a b]          a command with two arguments, "echo" and "a b"
 *   |                     separates commands in a pipeline
 *   <[in] >[out] >>[log]  redirections, in the order they were written
 *   &                     at the end: run in the background
 *   (empty)               blank line
 *   error: ...            a syntax error whose message contains "..."
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/parser.h"

/* The variables the tests can see. */
static const char *test_lookup(const char *name, void *ctx)
{
    (void)ctx;
    static const char *vars[][2] = {
        {"HOME", "/home/test"},
        {"FOO", "bar"},
        {"EMPTY", ""},
        {"SPACED", "a b"},
        {"_under_1", "u"},
        {"?", "42"},
    };
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++)
        if (strcmp(vars[i][0], name) == 0)
            return vars[i][1];
    return NULL;
}

static void dump(const Pipeline *p, char *out, size_t n)
{
    size_t len = 0;
    out[0] = '\0';
#define ADD(...) len += (size_t)snprintf(out + len, len < n ? n - len : 0, __VA_ARGS__)
    for (size_t c = 0; c < p->ncmds; c++) {
        const Command *cmd = &p->cmds[c];
        if (c > 0)
            ADD(" | ");
        for (size_t a = 0; a < cmd->argc; a++)
            ADD("%s[%s]", a > 0 ? " " : "", cmd->argv[a]);
        for (size_t r = 0; r < cmd->nredirs; r++) {
            const char *op = cmd->redirs[r].kind == REDIR_IN    ? "<"
                             : cmd->redirs[r].kind == REDIR_OUT ? ">"
                                                                : ">>";
            ADD(" %s[%s]", op, cmd->redirs[r].path);
        }
    }
    if (p->background)
        ADD(" &");
#undef ADD
}

static const struct {
    const char *input;
    const char *want;
} cases[] = {
    /* plain words and whitespace */
    {"ls", "[ls]"},
    {"ls -l /tmp", "[ls] [-l] [/tmp]"},
    {"   echo    hi   ", "[echo] [hi]"},
    {"echo\ta\tb", "[echo] [a] [b]"},
    {"", "(empty)"},
    {"     ", "(empty)"},

    /* quoting */
    {"echo \"a  b\"", "[echo] [a  b]"},
    {"echo 'a  b'", "[echo] [a  b]"},
    {"echo \"\"", "[echo] []"},
    {"echo ''", "[echo] []"},
    {"echo a\"b c\"'d'e", "[echo] [ab cde]"},
    {"echo 'it\"s'", "[echo] [it\"s]"},
    {"echo \"it's\"", "[echo] [it's]"},
    {"echo '|' \"<\" '>>' \"&\"", "[echo] [|] [<] [>>] [&]"},
    {"echo 'a\\nb'", "[echo] [a\\nb]"},          /* no escapes in '' */
    {"echo \"a\\\"b\"", "[echo] [a\"b]"},         /* \" inside "" */
    {"echo \"a\\\\b\"", "[echo] [a\\b]"},         /* \\ inside "" */
    {"echo \"a\\nb\"", "[echo] [a\\nb]"},         /* other \ kept in "" */

    /* backslash outside quotes */
    {"echo a\\ b", "[echo] [a b]"},
    {"echo \\|", "[echo] [|]"},
    {"echo \\'x\\'", "[echo] ['x']"},
    {"echo \\$FOO", "[echo] [$FOO]"},
    {"echo \\\\", "[echo] [\\]"},

    /* variables */
    {"echo $FOO", "[echo] [bar]"},
    {"echo \"$FOO\"", "[echo] [bar]"},
    {"echo '$FOO'", "[echo] [$FOO]"},
    {"echo \"\\$FOO\"", "[echo] [$FOO]"},
    {"echo ${FOO}baz", "[echo] [barbaz]"},
    {"echo $FOObaz", "[echo]"},                   /* $FOObaz is unset */
    {"echo x$FOO.y", "[echo] [xbar.y]"},
    {"echo $?", "[echo] [42]"},
    {"echo \"status=$?\"", "[echo] [status=42]"},
    {"echo ${?}", "[echo] [42]"},
    {"echo $_under_1", "[echo] [u]"},
    {"echo $UNSET end", "[echo] [end]"},          /* empty word dropped */
    {"echo \"$UNSET\" end", "[echo] [] [end]"},   /* but kept if quoted */
    {"echo $EMPTY", "[echo]"},
    {"echo $SPACED", "[echo] [a b]"},             /* no word splitting */
    {"echo $ a$ $1", "[echo] [$] [a$] [$1]"},
    {"$UNSET", "(empty)"},
    {"cd $HOME/src", "[cd] [/home/test/src]"},

    /* pipes */
    {"a | b", "[a] | [b]"},
    {"a|b|c", "[a] | [b] | [c]"},
    {"cat f | grep -v x | sort -r | head -n 3",
     "[cat] [f] | [grep] [-v] [x] | [sort] [-r] | [head] [-n] [3]"},

    /* redirection */
    {"sort < in.txt", "[sort] <[in.txt]"},
    {"echo hi > out.txt", "[echo] [hi] >[out.txt]"},
    {"echo hi >> log", "[echo] [hi] >>[log]"},
    {"echo hi>out", "[echo] [hi] >[out]"},
    {"> out echo hi", "[echo] [hi] >[out]"},
    {"sort <in >out", "[sort] <[in] >[out]"},
    {"echo a > one > two", "[echo] [a] >[one] >[two]"},
    {"echo > \"my file\"", "[echo] >[my file]"},
    {"cat < $HOME/f", "[cat] <[/home/test/f]"},
    {"a < in | b | c >> out", "[a] <[in] | [b] | [c] >>[out]"},

    /* background */
    {"sleep 5 &", "[sleep] [5] &"},
    {"sleep 5&", "[sleep] [5] &"},
    {"a | b &", "[a] | [b] &"},

    /* syntax errors */
    {"echo \"abc", "error: unterminated double quote"},
    {"echo 'abc", "error: unterminated single quote"},
    {"echo abc\\", "error: backslash at end of line"},
    {"| a", "error: unexpected '|'"},
    {"a |", "error: missing command after '|'"},
    {"a | | b", "error: unexpected '|'"},
    {"a ||", "error: unexpected '|'"},
    {"echo >", "error: missing file name after '>'"},
    {"echo >>", "error: missing file name after '>>'"},
    {"cat <", "error: missing file name after '<'"},
    {"echo > | x", "error: missing file name after '>'"},
    {"echo > > x", "error: missing file name after '>'"},
    {"> out", "error: missing command"},
    {"< in | b", "error: missing command before '|'"},
    {"&", "error: unexpected '&'"},
    {"a & b", "error: '&' is only allowed at the end"},
    {"a && b", "error: '&' is only allowed at the end"},
    {"a | &", "error: missing command after '|'"},
    {"echo ${FOO", "error: missing '}'"},
    {"echo ${}", "error: bad substitution"},
    {"echo ${1x}", "error: bad substitution"},
};

int main(void)
{
    int failed = 0;
    size_t n = sizeof cases / sizeof cases[0];
    for (size_t i = 0; i < n; i++) {
        Pipeline p;
        char err[128];
        char got[512];
        int r = parse_line(cases[i].input, test_lookup, NULL, &p, err, sizeof err);
        if (r == PARSE_OK) {
            dump(&p, got, sizeof got);
            pipeline_free(&p);
        } else if (r == PARSE_EMPTY) {
            snprintf(got, sizeof got, "(empty)");
        } else {
            snprintf(got, sizeof got, "error: %s", err);
        }

        const char *want = cases[i].want;
        int ok = strncmp(want, "error: ", 7) == 0
                     ? strncmp(got, "error: ", 7) == 0 && strstr(got, want + 7) != NULL
                     : strcmp(got, want) == 0;
        if (!ok) {
            failed++;
            fprintf(stderr, "FAIL  input: %s\n      want:  %s\n      got:   %s\n",
                    cases[i].input, want, got);
        }
    }

    /* a few things the table cannot express */
    int extra = 0, extra_failed = 0;
    Pipeline p;
    char err[64];

    /* argv is NULL-terminated, as execvp needs */
    extra++;
    if (parse_line("a b c", test_lookup, NULL, &p, err, sizeof err) != PARSE_OK ||
        p.cmds[0].argc != 3 || p.cmds[0].argv[3] != NULL)
        extra_failed++;
    else
        pipeline_free(&p);

    /* no lookup function: every variable is unset */
    extra++;
    if (parse_line("echo $HOME x", NULL, NULL, &p, err, sizeof err) != PARSE_OK ||
        p.cmds[0].argc != 2 || strcmp(p.cmds[0].argv[1], "x") != 0)
        extra_failed++;
    else
        pipeline_free(&p);

    /* errlen 0 must not write to err */
    extra++;
    char untouched[4] = "abc";
    if (parse_line("a |", test_lookup, NULL, &p, untouched, 0) != PARSE_ERROR ||
        strcmp(untouched, "abc") != 0)
        extra_failed++;

    /* a long message is cut to fit the buffer, not overflowed */
    extra++;
    char tiny[8];
    if (parse_line("echo 'x", test_lookup, NULL, &p, tiny, sizeof tiny) != PARSE_ERROR ||
        strlen(tiny) != sizeof tiny - 1)
        extra_failed++;

    /* a long line with many words and pipes */
    extra++;
    char big[8192];
    size_t len = 0;
    for (int i = 0; i < 400; i++)
        len += (size_t)snprintf(big + len, sizeof big - len, "w%d%s", i,
                                i % 40 == 39 && i != 399 ? " | " : " ");
    if (parse_line(big, test_lookup, NULL, &p, err, sizeof err) != PARSE_OK ||
        p.ncmds != 10 || p.cmds[9].argc != 40 ||
        strcmp(p.cmds[9].argv[39], "w399") != 0)
        extra_failed++;
    else
        pipeline_free(&p);

    if (extra_failed > 0)
        fprintf(stderr, "%d of the extra checks failed\n", extra_failed);
    printf("%zu table cases + %d extra checks, %d failed\n", n, extra,
           failed + extra_failed);
    return failed + extra_failed == 0 ? 0 : 1;
}
