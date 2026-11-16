<img src="assets/jd-logo.png" width="96" alt="JD logo">

# mini-shell

[![ci](https://github.com/AyerPrabin/mini-shell/actions/workflows/ci.yml/badge.svg)](https://github.com/AyerPrabin/mini-shell/actions/workflows/ci.yml)

A small Unix shell written in C11 for Linux. It reads a line, splits it into
commands, and runs them with `fork`, `execvp` and `waitpid`, with pipes,
redirection, a few built-ins and background jobs.

```
msh$ export NAME=world
msh$ echo "hello $NAME" | tr a-z A-Z > out.txt
msh$ cat < out.txt
HELLO WORLD
msh$ sleep 10 &
[1] 4821
msh$ jobs
[1] Running    sleep 10
```

## Why

In my operating systems module we learnt that a shell "forks and then
execs", and that pipes are file descriptors. I could repeat that, but I
could not have said how `ls | wc -l` actually gets wired up, who closes
which end of the pipe, or why Ctrl-C stops the program and not the shell.
Writing a shell was the most direct way to find out. It is deliberately
small; bash is far more complete.

## What works

- A prompt (`msh$ `) when stdin is a terminal. When stdin is a pipe or a
  file it just runs the lines, which is how the tests drive it.
- Running programs found on `PATH`, with `127` for "command not found" and
  `126` when the file is not executable.
- Quoting: `"..."`, `'...'` and backslash escapes.
- Variables: `$NAME`, `${NAME}` and `$?` (exit status of the last command).
- Pipes of any length: `a | b | c`.
- Redirection: `< file`, `> file`, `>> file`, anywhere in a command.
- Built-ins: `cd` (also sets `PWD` and `OLDPWD`), `pwd`, `exit [n]`,
  `export NAME=value`, `unset NAME`, and `jobs`.
- Background jobs with `&`. Finished jobs are reaped before each prompt, so
  they do not stay around as zombies, and the shell prints `[n] Done` for
  them when it is interactive.
- Ctrl-C stops the program in the foreground but not the shell or the
  background jobs. Ctrl-C at an empty prompt just gives a new prompt.

## Building and running

You need Linux, `make` and gcc or clang.

```sh
make            # builds ./msh
./msh
make test       # parser unit tests + integration tests
make sanitize   # the same, built with -fsanitize=address,undefined
make valgrind   # parser unit tests under Valgrind
```

Everything is compiled with
`-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic`.

## Code layout

| File | What it does |
|------|--------------|
| `src/parser.c` | turns a line into a `Pipeline` struct; no system calls |
| `src/exec.c` | forks, sets up pipes and redirections, execs, waits; background jobs |
| `src/builtins.c` | `cd`, `pwd`, `exit`, `export`, `unset`, `jobs` |
| `src/main.c` | the read-parse-run loop and the SIGINT handler |

## How parsing works

The parser is kept apart from everything that touches the operating
system. It never calls `getenv`; the shell passes it a lookup function, and
the unit tests pass one with a fixed set of variables. That makes it
possible to test all the fiddly cases quickly and without forking anything.

It works in two passes:

1. **Lexing** walks the line one character at a time and produces tokens:
   words and the operators `|`, `<`, `>`, `>>` and `&`. Quotes, escapes and
   variables are dealt with here, so each word token is already the final
   argument.
   - Inside `'...'` everything is literal, including `\` and `$`.
   - Inside `"..."`, `$NAME` is expanded, and `\` only escapes `"`, `\` and
     `$` (any other backslash is kept as is, like in sh).
   - Outside quotes, `\` makes the next character plain, so `a\ b` is one
     word and `\|` is not a pipe.
   - Quoted and unquoted pieces next to each other join into one word:
     `a"b c"'d'` is `ab cd`.
   - An unquoted variable that is unset or empty disappears completely, but
     `""` or `"$UNSET"` gives an empty argument.
2. **Parsing** groups the tokens into commands split on `|`, takes the word
   after each `<`, `>` or `>>` as a file name, and rejects things that do not
   make sense: an empty command (`| a`, `a |`, `a | | b`), a redirection with
   no file, or `&` anywhere but the end. Syntax errors set `$?` to 2.

The result is a `Pipeline`: an array of `Command`s, each with a
NULL-terminated `argv` ready for `execvp` and a list of redirections, plus a
`background` flag.

## How pipes and redirection are wired

For `a | b | c` the shell makes two pipes and forks three children:

```
a:  stdout -> pipe1[write]
b:  stdin  <- pipe1[read]    stdout -> pipe2[write]
c:  stdin  <- pipe2[read]
```

Each child uses `dup2(fd, 0)` or `dup2(fd, 1)` to put the pipe end in place
of its stdin or stdout, closes the original descriptors, and then calls
`execvp`. The program it runs has no idea it is in a pipe; it just reads fd 0
and writes fd 1.

The part that is easy to get wrong is closing. A reader only sees
end-of-file when *every* copy of the write end is closed, including the
copies inherited by the shell and the other children. So the parent closes
each pipe end as soon as the child that needs it has been forked, and only
ever has the read end of the previous pipe open while it forks the next
command. If one copy stays open, `b` waits forever for more input.

Redirections are applied in the child after the pipe set-up, so `a > f | b`
sends a's output to the file, the same as in bash. `<` opens the file
read-only, `>` with `O_TRUNC`, `>>` with `O_APPEND`, and then the same
`dup2` trick moves it onto fd 0 or 1.

Built-ins are special. `cd` has to change the shell's own directory, so a
built-in on its own runs inside the shell process. If it has a redirection,
the shell saves fd 0 and 1 with `dup`, applies the redirection, runs the
built-in, flushes stdout and puts the saved descriptors back. A built-in
inside a pipeline runs in a child like any other command, so `cd /tmp | true`
does not change directory (bash behaves the same way).

The shell also calls `fflush(stdout)` before every `fork`. Otherwise output
still sitting in its stdio buffer is copied into the child and can come out
twice, or out of order, when stdout is a pipe.

## Signals and background jobs

When you press Ctrl-C the terminal sends SIGINT to every process in the
foreground process group, which is the shell and whatever it is running.
The shell installs a handler that does nothing, without `SA_RESTART`, so the
signal only interrupts its `waitpid` or `getline`. Children reset SIGINT to
the default before `execvp`, so they are stopped. The `$?` after a Ctrl-C is
130 (128 + SIGINT), as in bash.

A background job is put in its own process group with `setpgid`, so the
Ctrl-C does not reach it, and its stdin is pointed at `/dev/null` so it
cannot steal the shell's input. Before reading each line the shell calls
`waitpid(-1, ..., WNOHANG)` in a loop to collect any background processes
that have finished.

One bug I only found in CI: `seq 1 100000 | head -n 1` printed
`seq: write error: Broken pipe`. The GitHub runner starts processes with
SIGPIPE *ignored*, and an ignored signal stays ignored across `exec`, so
`seq` got an error instead of being killed when `head` exited. Children now
reset SIGPIPE to the default as well.

## Tests

**Parser unit tests** (`tests/test_parser.c`): a table of 78 input lines and
the expected parse, written in a short text form such as
`[echo] [a b] | [wc] >[out]`, plus 5 extra checks (NULL-terminated argv, no
lookup function, error buffer limits, a long line). They cover quoting edge
cases, escapes inside and outside quotes, variable expansion, empty words,
pipes, redirections in odd places, `&`, and 21 syntax errors. They run
normally, under ASan/UBSan, and under Valgrind.

```
78 table cases + 5 extra checks, 0 failed
```

**Integration tests** (`tests/integration.sh`): 47 cases that pipe lines
into the built `msh` and compare everything it prints with the expected
text. They cover commands, quoting, variables, every built-in, exit codes,
pipes (including 200,000 lines through a four-stage pipe), all the redirections,
syntax errors, background jobs and reaping, and Ctrl-C. The Ctrl-C test
starts the shell under `setsid`, sends SIGINT to its process group while
`sleep 30` runs and again while it waits for input, and checks that the
shell carries on, `$?` is 130 and a background job survives.

```
47 passed, 0 failed
```

CI runs all of this on Ubuntu with gcc and clang, and runs the integration
tests a second time against the sanitizer build.

## Known limitations

- No job control: there is no `fg`, `bg`, Ctrl-Z, or terminal hand-over
  with `tcsetpgrp`. Foreground commands share the shell's process group.
- No globbing (`*.c` is passed through as-is), no `~` expansion.
- No here-documents (`<<`).
- No `;`, `&&` or `||`, and no subshells or `$(...)`.
- No file descriptor numbers in redirections (`2>`, `2>&1`).
- Variables are not split into words: if `X="a b"`, `$X` is one argument.
- No `NAME=value command` prefix assignments; use `export`.
- Built-ins only change the shell when they run on their own (see above).
- A backslash at the end of a line is an error, not a line continuation.
- The shell reads stdin through stdio, which reads ahead. When it is reading
  commands from a pipe, a foreground program that reads stdin can miss input
  the shell has already buffered. bash avoids this by reading one byte at a
  time from pipes.
- The prompt, `[n] Done` messages and Ctrl-C at a terminal are only
  partly covered by the tests, which run the shell non-interactively.
- No line editing or history.

## What I would add next

- Job control: give each pipeline its own process group, hand the terminal
  to it with `tcsetpgrp`, handle SIGTSTP, and add `fg` and `bg`.
- `;`, `&&` and `||`, which needs the parser to return a list of pipelines.
- `2>` and `2>&1`.
- Globbing with `glob(3)`.
- Here-documents.

## Licence

MIT, see [LICENSE](LICENSE).
