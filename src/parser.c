/*
 * Parsing happens in two passes:
 *
 *   1. lex(): walk the characters and produce tokens. This is where quotes,
 *      backslashes and $VAR expansion are handled, so a token is already the
 *      final text of a word.
 *   2. parse_tokens(): group the tokens into commands separated by |, pick
 *      out the redirections, and check the order of things (no empty
 *      commands, a file name after every < or >, & only at the end).
 *
 * Only malloc/free and string functions are used here; no system calls.
 */
#include "parser.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- small growable string ---------- */

typedef struct {
    char *s;
    size_t len, cap;
    int oom;
} Buf;

static void buf_putc(Buf *b, char c)
{
    if (b->oom)
        return;
    if (b->len + 2 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 32;
        char *p = realloc(b->s, cap);
        if (p == NULL) {
            b->oom = 1;
            return;
        }
        b->s = p;
        b->cap = cap;
    }
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s)
{
    while (*s != '\0')
        buf_putc(b, *s++);
}

/* ---------- tokens ---------- */

typedef enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_AMP } TokType;

typedef struct {
    TokType type;
    char *text; /* only for T_WORD */
} Token;

typedef struct {
    Token *v;
    size_t len, cap;
} TokList;

static void toks_free(TokList *t)
{
    for (size_t i = 0; i < t->len; i++)
        free(t->v[i].text);
    free(t->v);
}

static int toks_push(TokList *t, TokType type, char *text)
{
    if (t->len == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 8;
        Token *p = realloc(t->v, cap * sizeof *p);
        if (p == NULL)
            return -1;
        t->v = p;
        t->cap = cap;
    }
    t->v[t->len].type = type;
    t->v[t->len].text = text;
    t->len++;
    return 0;
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (errlen == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static const char *tok_name(TokType t)
{
    switch (t) {
    case T_PIPE: return "|";
    case T_IN: return "<";
    case T_OUT: return ">";
    case T_APPEND: return ">>";
    case T_AMP: return "&";
    default: return "word";
    }
}

/* ---------- lexer ---------- */

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_operator(char c)
{
    return c == '|' || c == '<' || c == '>' || c == '&';
}

static int is_name_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static int is_name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/*
 * line[*i] is a '$'. Appends the expansion to b and moves *i past it.
 * Returns 1 if the '$' turned out to be literal (not followed by a name),
 * 0 after a normal expansion, -1 on a syntax error.
 */
static int expand_var(const char *line, size_t *i, Buf *b, var_lookup_fn lookup,
                      void *ctx, char *err, size_t errlen)
{
    size_t j = *i + 1;
    Buf name = {0};

    if (line[j] == '?') {
        buf_putc(&name, '?');
        j++;
    } else if (line[j] == '{') {
        j++;
        while (line[j] != '\0' && line[j] != '}')
            buf_putc(&name, line[j++]);
        if (line[j] != '}') {
            free(name.s);
            set_err(err, errlen, "missing '}' after '${'");
            return -1;
        }
        j++;
        int ok = name.len > 0 && (strcmp(name.s, "?") == 0 || is_name_start(name.s[0]));
        for (size_t k = 1; ok && k < name.len; k++)
            ok = is_name_char(name.s[k]);
        if (!ok) {
            set_err(err, errlen, "bad substitution '${%s}'", name.s ? name.s : "");
            free(name.s);
            return -1;
        }
    } else if (is_name_start(line[j])) {
        while (is_name_char(line[j]))
            buf_putc(&name, line[j++]);
    } else {
        /* "$" on its own, "$5", "$-" ...: keep the dollar sign as text */
        buf_putc(b, '$');
        *i += 1;
        return 1;
    }

    if (name.oom) {
        free(name.s);
        b->oom = 1;
        return 0;
    }
    const char *value = lookup != NULL ? lookup(name.s, ctx) : NULL;
    if (value != NULL)
        buf_puts(b, value);
    free(name.s);
    *i = j;
    return 0;
}

static int lex(const char *line, var_lookup_fn lookup, void *ctx, TokList *out,
               char *err, size_t errlen)
{
    size_t i = 0;
    for (;;) {
        while (is_space(line[i]))
            i++;
        char c = line[i];
        if (c == '\0')
            return 0;

        if (is_operator(c)) {
            TokType t = c == '|' ? T_PIPE : c == '<' ? T_IN : c == '&' ? T_AMP : T_OUT;
            i++;
            if (t == T_OUT && line[i] == '>') {
                t = T_APPEND;
                i++;
            }
            if (toks_push(out, t, NULL) != 0)
                goto oom;
            continue;
        }

        /* A word runs until unquoted whitespace or an operator. Quoted and
         * unquoted parts next to each other join up: a"b c"'d' is one word. */
        Buf b = {0};
        int keep = 0; /* keep the word even if it ends up empty, as in "" */
        while (line[i] != '\0' && !is_space(line[i]) && !is_operator(line[i])) {
            c = line[i];
            if (c == '\'') {
                /* single quotes: everything literal, no escapes at all */
                keep = 1;
                i++;
                while (line[i] != '\0' && line[i] != '\'')
                    buf_putc(&b, line[i++]);
                if (line[i] != '\'') {
                    free(b.s);
                    set_err(err, errlen, "unterminated single quote");
                    return -1;
                }
                i++;
            } else if (c == '"') {
                /* double quotes: $VAR expands, \ only escapes " \ and $ */
                keep = 1;
                i++;
                while (line[i] != '"') {
                    if (line[i] == '\0') {
                        free(b.s);
                        set_err(err, errlen, "unterminated double quote");
                        return -1;
                    }
                    if (line[i] == '\\' && (line[i + 1] == '"' || line[i + 1] == '\\' ||
                                            line[i + 1] == '$')) {
                        buf_putc(&b, line[i + 1]);
                        i += 2;
                    } else if (line[i] == '$') {
                        if (expand_var(line, &i, &b, lookup, ctx, err, errlen) < 0) {
                            free(b.s);
                            return -1;
                        }
                    } else {
                        buf_putc(&b, line[i++]);
                    }
                }
                i++;
            } else if (c == '\\') {
                /* outside quotes a backslash makes the next character plain */
                if (line[i + 1] == '\0') {
                    free(b.s);
                    set_err(err, errlen, "backslash at end of line");
                    return -1;
                }
                keep = 1;
                buf_putc(&b, line[i + 1]);
                i += 2;
            } else if (c == '$') {
                int r = expand_var(line, &i, &b, lookup, ctx, err, errlen);
                if (r < 0) {
                    free(b.s);
                    return -1;
                }
                if (r == 1)
                    keep = 1;
            } else {
                keep = 1;
                buf_putc(&b, c);
                i++;
            }
        }
        if (b.oom) {
            free(b.s);
            goto oom;
        }
        /* An unquoted $UNSET on its own disappears, like in sh. */
        if (!keep && b.len == 0) {
            free(b.s);
            continue;
        }
        if (b.s == NULL) {
            b.s = malloc(1);
            if (b.s == NULL)
                goto oom;
            b.s[0] = '\0';
        }
        if (toks_push(out, T_WORD, b.s) != 0) {
            free(b.s);
            goto oom;
        }
    }
oom:
    set_err(err, errlen, "out of memory");
    return -1;
}

/* ---------- parser ---------- */

static void command_free(Command *c)
{
    for (size_t i = 0; i < c->argc; i++)
        free(c->argv[i]);
    free(c->argv);
    for (size_t i = 0; i < c->nredirs; i++)
        free(c->redirs[i].path);
    free(c->redirs);
}

void pipeline_free(Pipeline *p)
{
    for (size_t i = 0; i < p->ncmds; i++)
        command_free(&p->cmds[i]);
    free(p->cmds);
    p->cmds = NULL;
    p->ncmds = 0;
}

/* Words are moved out of the token list (the token's text is set to NULL),
 * so each string has exactly one owner at any time. */
static int add_arg(Command *c, char **word)
{
    char **argv = realloc(c->argv, (c->argc + 2) * sizeof *argv);
    if (argv == NULL)
        return -1;
    c->argv = argv;
    argv[c->argc++] = *word;
    argv[c->argc] = NULL;
    *word = NULL;
    return 0;
}

static int add_redir(Command *c, RedirKind kind, char **path)
{
    Redir *r = realloc(c->redirs, (c->nredirs + 1) * sizeof *r);
    if (r == NULL)
        return -1;
    c->redirs = r;
    r[c->nredirs].kind = kind;
    r[c->nredirs].path = *path;
    c->nredirs++;
    *path = NULL;
    return 0;
}

static int add_command(Pipeline *p, Command *c)
{
    Command *cmds = realloc(p->cmds, (p->ncmds + 1) * sizeof *cmds);
    if (cmds == NULL)
        return -1;
    p->cmds = cmds;
    cmds[p->ncmds++] = *c;
    memset(c, 0, sizeof *c);
    return 0;
}

static int parse_tokens(TokList *t, Pipeline *p, char *err, size_t errlen)
{
    Command cur = {0};
    for (size_t i = 0; i < t->len; i++) {
        Token *tok = &t->v[i];
        switch (tok->type) {
        case T_WORD:
            if (add_arg(&cur, &tok->text) != 0)
                goto oom;
            break;
        case T_IN:
        case T_OUT:
        case T_APPEND:
            if (i + 1 >= t->len || t->v[i + 1].type != T_WORD) {
                set_err(err, errlen, "missing file name after '%s'", tok_name(tok->type));
                goto fail;
            }
            i++;
            if (add_redir(&cur,
                          tok->type == T_IN    ? REDIR_IN
                          : tok->type == T_OUT ? REDIR_OUT
                                               : REDIR_APPEND,
                          &t->v[i].text) != 0)
                goto oom;
            break;
        case T_PIPE:
            if (cur.argc == 0) {
                set_err(err, errlen, cur.nredirs > 0 ? "missing command before '|'"
                                                     : "unexpected '|'");
                goto fail;
            }
            if (add_command(p, &cur) != 0)
                goto oom;
            break;
        case T_AMP:
            if (i + 1 != t->len) {
                set_err(err, errlen, "'&' is only allowed at the end of a line");
                goto fail;
            }
            if (cur.argc == 0 && p->ncmds == 0 && cur.nredirs == 0) {
                set_err(err, errlen, "unexpected '&'");
                goto fail;
            }
            p->background = 1;
            break;
        }
    }
    if (cur.argc == 0) {
        if (p->ncmds > 0)
            set_err(err, errlen, "missing command after '|'");
        else
            set_err(err, errlen, "missing command");
        goto fail;
    }
    if (add_command(p, &cur) != 0)
        goto oom;
    return 0;

oom:
    set_err(err, errlen, "out of memory");
fail:
    command_free(&cur);
    pipeline_free(p);
    return -1;
}

int parse_line(const char *line, var_lookup_fn lookup, void *ctx,
               Pipeline *out, char *err, size_t errlen)
{
    TokList toks = {0};
    memset(out, 0, sizeof *out);
    if (errlen > 0)
        err[0] = '\0';

    if (lex(line, lookup, ctx, &toks, err, errlen) != 0) {
        toks_free(&toks);
        return PARSE_ERROR;
    }
    if (toks.len == 0) {
        toks_free(&toks);
        return PARSE_EMPTY;
    }
    int r = parse_tokens(&toks, out, err, errlen);
    toks_free(&toks);
    return r == 0 ? PARSE_OK : PARSE_ERROR;
}
