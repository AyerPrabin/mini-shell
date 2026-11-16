#!/bin/sh
# Integration tests: feed lines to the built shell on stdin and compare what
# it prints (stdout and stderr together) with what we expect.
#
# usage: sh tests/integration.sh ./msh

set -u

if [ $# -ne 1 ]; then
    echo "usage: $0 path/to/msh" >&2
    exit 2
fi
MSH=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

passed=0
failed=0

# check NAME INPUT EXPECTED
check() {
    name=$1
    input=$2
    expected=$3
    actual=$(cd "$WORK" && printf '%s\n' "$input" | "$MSH" 2>&1)
    if [ "$actual" = "$expected" ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        echo "FAIL: $name"
        echo "  input:"
        printf '%s\n' "$input" | sed 's/^/    /'
        echo "  expected:"
        printf '%s\n' "$expected" | sed 's/^/    /'
        echo "  actual:"
        printf '%s\n' "$actual" | sed 's/^/    /'
    fi
}

# check_exit NAME INPUT CODE: the shell's own exit status
check_exit() {
    (cd "$WORK" && printf '%s\n' "$2" | "$MSH" >/dev/null 2>&1)
    code=$?
    if [ "$code" = "$3" ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        echo "FAIL: $1 (exit status $code, expected $3)"
    fi
}

# ---- running programs ----

check "simple command" \
'echo hello world' \
'hello world'

check "several lines" \
'echo one
echo two' \
'one
two'

check "blank lines are ignored" \
'

echo after-blank' \
'after-blank'

check "command not found" \
'no-such-command-xyz
echo "status $?"' \
'mini-shell: no-such-command-xyz: command not found
status 127'

check "not executable" \
'printf "x" > notexec
./notexec
echo "status $?"' \
'mini-shell: ./notexec: Permission denied
status 126'

check "exit status of false and true" \
'false
echo $?
true
echo $?' \
'1
0'

# ---- quoting ----

check "double and single quotes" \
'echo "a   b" '"'"'c   d'"'"' e\ \ f' \
'a   b c   d e  f'

check "quotes glue words together" \
'echo pre"mid dle"'"'"'post'"'" \
'premid dlepost'

check "operators inside quotes are plain text" \
'echo "a | b > c" '"'"'&'"'" \
'a | b > c &'

check "empty argument is kept" \
'printf "[%s]\n" "" x' \
'[]
[x]'

# ---- variables ----

check "export and expand" \
'export GREETING=hi NAME=world
echo "$GREETING, $NAME" ${GREETING}there '"'"'$NAME'"'" \
'hi, world hithere $NAME'

check "exported variables reach child programs" \
'export MSH_TEST_VAR=from-msh
printenv MSH_TEST_VAR' \
'from-msh'

check "unset" \
'export GONE=1
unset GONE
echo "[$GONE]"
printenv GONE
echo $?' \
'[]
1'

check "bad identifiers" \
'export 1abc=x
echo $?
unset a-b
echo $?' \
"mini-shell: export: '1abc=x': not a valid identifier
1
mini-shell: unset: 'a-b': not a valid identifier
1"

check "unset variable disappears, quoted one stays" \
'printf "[%s]" $NOPE_NOT_SET "$NOPE_NOT_SET" end
echo' \
'[][end]'

check "no word splitting of variables" \
'export TWO="a b"
printf "<%s>\n" $TWO' \
'<a b>'

# ---- built-ins ----

check "cd and pwd" \
'cd /
pwd
cd /usr
pwd
echo $PWD $OLDPWD' \
'/
/usr
/usr /'

check "cd with no argument goes to HOME" \
'export HOME=/tmp
cd
pwd' \
'/tmp'

check "cd to a missing directory" \
'cd /no/such/dir
echo "status $?"' \
'mini-shell: cd: /no/such/dir: No such file or directory
status 1'

check "cd in a pipeline does not change the shell" \
'cd /
cd /usr | true
pwd' \
'/'

check "exit stops reading" \
'echo before
exit
echo after' \
'before'

check_exit "exit with a code" 'exit 3' 3
check_exit "exit uses last status" 'false
exit' 1
check_exit "exit at end of input uses last status" 'false' 1
check_exit "exit 256 wraps to 0" 'exit 256' 0
check_exit "exit with a bad number" 'exit abc' 2

# ---- pipes ----

check "two-stage pipe" \
'printf "b\na\nc\n" | sort' \
'a
b
c'

check "three-stage pipe" \
'printf "b\na\nc\n" | sort | head -n 2' \
'a
b'

check "long pipe" \
'seq 1 1000 | grep 7 | sort -n | tail -n 3 | wc -l' \
'3'

check "lots of data through a pipe" \
'seq 1 200000 | cat | cat | tail -n 1' \
'200000'

check "count matches through a pipe" \
'seq 1 1000 | grep 7 | wc -l' \
'271'

check "status of a pipe is the last command" \
'false | true
echo $?
true | false
echo $?' \
'0
1'

check "built-in inside a pipe" \
'cd /
pwd | cat' \
'/'

check "reader finishes early" \
'seq 1 100000 | head -n 1' \
'1'

# ---- redirection ----

check "output, append and input" \
'echo one > f.txt
echo two >> f.txt
cat < f.txt
wc -l < f.txt' \
'one
two
2'

check "> truncates" \
'echo long-long-line > t.txt
echo x > t.txt
cat t.txt' \
'x'

check "redirect in the middle of a command" \
'echo a > mid.txt b c
cat mid.txt' \
'a b c'

check "redirect with a pipe" \
'printf "3\n1\n2\n" > nums.txt
sort < nums.txt | head -n 1 > first.txt
cat first.txt' \
'1'

check "redirect a built-in" \
'cd /
pwd > '"$WORK"'/pwd.txt
cd '"$WORK"'
cat pwd.txt
echo still-on-stdout' \
'/
still-on-stdout'

check "missing input file" \
'cat < missing.txt
echo "status $?"' \
'mini-shell: missing.txt: No such file or directory
status 1'

check "last of several redirects wins" \
'echo hi > r1.txt > r2.txt
cat r2.txt
wc -c < r1.txt' \
'hi
0'

# ---- syntax errors ----

check "syntax errors" \
'echo "unterminated
echo $?
| wc
echo $?
ls >
echo $?' \
'mini-shell: syntax error: unterminated double quote
2
mini-shell: syntax error: unexpected '"'"'|'"'"'
2
mini-shell: syntax error: missing file name after '"'"'>'"'"'
2'

# ---- background jobs ----

check "background job runs while the shell carries on" \
'sh -c "sleep 0.3; echo background-done" &
echo foreground-first
sleep 0.6' \
'foreground-first
background-done'

check "finished background jobs are reaped" \
'sleep 0.2 &
sleep 1 &
jobs
sleep 0.5
jobs' \
'[1] Running    sleep 0.2
[2] Running    sleep 1
[2] Running    sleep 1'

check "background job does not read the shell's input" \
'cat &
echo still-here' \
'still-here'

check "background status is 0" \
'false &
echo $?' \
'0'

# ---- Ctrl-C ----
# A real terminal sends SIGINT to the whole foreground process group. Here
# the shell runs under setsid so it leads its own group, and we send SIGINT
# to that group with kill. The sleeps are generous so the order of the
# output does not depend on timing.

sigint_test() {
    fifo="$WORK/fifo"
    out="$WORK/sigint.out"
    rm -f "$fifo"
    mkfifo "$fifo"
    (cd "$WORK" && setsid "$MSH" < "$fifo" > "$out" 2>&1) &
    exec 3> "$fifo"
    sleep 0.3
    pid=$(pgrep -f -n "^$MSH\$")
    if [ -z "$pid" ]; then
        exec 3>&-
        wait
        failed=$((failed + 1))
        echo "FAIL: Ctrl-C (could not find the shell process)"
        return
    fi

    # a background job, which must survive the Ctrl-C
    echo 'sh -c "sleep 2; echo background-survived" &' >&3
    # Ctrl-C while a foreground program runs
    echo 'sleep 30' >&3
    sleep 0.5
    env kill -s INT -- "-$pid"
    sleep 0.3
    echo 'echo "after fg ctrl-c: $?"' >&3
    sleep 0.3
    # Ctrl-C while the shell is waiting for input
    env kill -s INT -- "-$pid"
    sleep 0.3
    echo 'echo "after idle ctrl-c: $?"' >&3
    echo 'sleep 2.5' >&3
    exec 3>&-
    wait

    expected='after fg ctrl-c: 130
after idle ctrl-c: 130
background-survived'
    actual=$(cat "$out")
    if [ "$actual" = "$expected" ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        echo "FAIL: Ctrl-C"
        echo "  expected:"
        printf '%s\n' "$expected" | sed 's/^/    /'
        echo "  actual:"
        printf '%s\n' "$actual" | sed 's/^/    /'
    fi
}

if command -v setsid >/dev/null 2>&1 && command -v pgrep >/dev/null 2>&1; then
    sigint_test
else
    echo "skipping Ctrl-C test (needs setsid and pgrep)"
fi

echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
