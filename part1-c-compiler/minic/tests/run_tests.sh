#!/usr/bin/env bash
# Test runner for minic.
#
# Every file in tests/cases/*.c is one test. Its expectations are written in
# header comments:
#
#   // TEST: <short explanation of what the test checks>
#   // EXPECT-EXIT: <n>            exit code of the compiled program (0-255)
#   // EXPECT-STDOUT: a|b|c        program output; '|' separates lines
#   // EXPECT-ERROR: <substring>   minic must fail and print this (repeatable)
#   // EXPECT-ASM-O1: <substring>      must appear in the -O1 assembly (repeatable)
#   // EXPECT-ASM-O1-NOT: <substring>  must not appear in the -O1 assembly
#   // EXPECT-IR-OPT: <substring>      must appear in the optimized IR
#   // EXPECT-IR-OPT-NOT: <substring>  must not appear in the optimized IR
#   // COMPILE-ONLY                do not assemble/link/run
#   // NO-GCC                      skip the differential test against gcc
#
# For a valid program the runner:
#   1. compiles it with minic at -O0 and at -O1, assembles with `as`, links
#      with `gcc`, runs it, and checks exit code and stdout at both levels
#      (so -O0 and -O1 must behave identically);
#   2. compiles the same source with real gcc (plus a print() shim) and checks
#      that gcc's program behaves the same (differential testing).
#
# The examples in part1-c-compiler/examples are also run through steps 1-2.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
MINIC="$REPO/build/minic"
EXAMPLES="$REPO/part1-c-compiler/examples"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

if [ ! -x "$MINIC" ]; then
    echo "build/minic not found. Run 'make build' first."
    exit 1
fi

directive() { # directive FILE NAME -> values, one per line
    sed -n "s|^// $2: \{0,1\}||p" "$1"
}
has_flag() { grep -q "^// $2\$" "$1"; }

report() { # report STATUS NAME EXPLANATION EXPECTED ACTUAL
    if [ "$1" = PASS ]; then
        pass=$((pass + 1))
        printf 'PASS  %-28s %s\n' "$2" "$3"
        printf '        expected: %s\n        actual:   %s\n' "$4" "$5"
    else
        fail=$((fail + 1))
        printf 'FAIL  %-28s %s\n' "$2" "$3"
        printf '        expected: %s\n        actual:   %s\n' "$4" "$5"
    fi
}

# build_and_run OPT SOURCE -> sets run_exit, run_out; returns 1 if the build failed
build_and_run() {
    local opt="$1" src="$2" base="$WORK/prog_O$1"
    "$MINIC" "-O$opt" -o "$base.s" "$src" 2>"$WORK/minic.err" || { run_out="minic failed: $(cat "$WORK/minic.err")"; return 1; }
    as "$base.s" -o "$base.o" 2>"$WORK/as.err" || { run_out="as failed: $(cat "$WORK/as.err")"; return 1; }
    gcc "$base.o" -o "$base" 2>"$WORK/ld.err" || { run_out="link failed: $(cat "$WORK/ld.err")"; return 1; }
    run_out="$(timeout 5 "$base")"
    run_exit=$?
    return 0
}

# gcc_run SOURCE -> sets gcc_exit, gcc_out
gcc_run() {
    {
        printf '#include <stdio.h>\nstatic void print(int x){printf("%%d\\n",x);}\n'
        cat "$1"
    } >"$WORK/gcc_prog.c"
    gcc -w -O0 -fwrapv "$WORK/gcc_prog.c" -o "$WORK/gcc_prog" 2>"$WORK/gcc.err" || { gcc_out="gcc failed: $(head -3 "$WORK/gcc.err")"; gcc_exit=-1; return 1; }
    gcc_out="$(timeout 5 "$WORK/gcc_prog")"
    gcc_exit=$?
}

flat() { printf '%s' "$1" | tr '\n' '|'; }

run_valid() { # run_valid NAME FILE EXPLANATION
    local name="$1" file="$2" why="$3"
    local want_exit want_out expected actual ok=1
    want_exit="$(directive "$file" EXPECT-EXIT | head -1)"
    want_out="$(directive "$file" EXPECT-STDOUT | head -1)"
    expected="exit=${want_exit:-any} stdout='${want_out}' at -O0 and -O1"
    actual=""

    if ! has_flag "$file" COMPILE-ONLY; then
        local o0_exit o0_out
        for opt in 0 1; do
            if ! build_and_run "$opt" "$file"; then
                report FAIL "$name" "$why" "$expected" "$run_out"
                return
            fi
            actual="$actual-O$opt: exit=$run_exit stdout='$(flat "$run_out")'  "
            [ -n "$want_exit" ] && [ "$run_exit" != "$want_exit" ] && ok=0
            [ -n "$want_out" ] && [ "$(flat "$run_out")" != "$want_out" ] && ok=0
            if [ "$opt" = 0 ]; then o0_exit=$run_exit; o0_out="$run_out"; fi
        done
        if [ "$run_exit" != "$o0_exit" ] || [ "$run_out" != "$o0_out" ]; then
            ok=0
            actual="$actual(-O0 and -O1 differ!) "
        fi
        if ! has_flag "$file" NO-GCC; then
            gcc_run "$file"
            actual="${actual}gcc: exit=$gcc_exit stdout='$(flat "$gcc_out")'"
            if [ "$gcc_exit" != "$o0_exit" ] || [ "$gcc_out" != "$o0_out" ]; then
                ok=0
                actual="$actual (differs from gcc!)"
            fi
        fi
    else
        "$MINIC" -O1 -o "$WORK/only.s" "$file" 2>"$WORK/minic.err" || { ok=0; actual="minic failed: $(cat "$WORK/minic.err")"; }
        actual="${actual}compiled only"
    fi

    # Checks on the optimized output.
    local asm ir pat
    asm="$("$MINIC" -O1 "$file" 2>/dev/null | grep -v '^ *#')"
    ir="$("$MINIC" --emit=ir-opt "$file" 2>/dev/null)"
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        expected="$expected; -O1 asm has '$pat'"
        grep -qF -- "$pat" <<<"$asm" || { ok=0; actual="$actual; asm lacks '$pat'"; }
    done < <(directive "$file" EXPECT-ASM-O1)
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        expected="$expected; -O1 asm has no '$pat'"
        grep -qF -- "$pat" <<<"$asm" && { ok=0; actual="$actual; asm contains '$pat'"; }
    done < <(directive "$file" EXPECT-ASM-O1-NOT)
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        expected="$expected; optimized IR has '$pat'"
        grep -qF -- "$pat" <<<"$ir" || { ok=0; actual="$actual; IR lacks '$pat'"; }
    done < <(directive "$file" EXPECT-IR-OPT)
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        expected="$expected; optimized IR has no '$pat'"
        grep -qF -- "$pat" <<<"$ir" && { ok=0; actual="$actual; IR contains '$pat'"; }
    done < <(directive "$file" EXPECT-IR-OPT-NOT)

    if [ $ok = 1 ]; then report PASS "$name" "$why" "$expected" "$actual"; else report FAIL "$name" "$why" "$expected" "$actual"; fi
}

run_error() { # run_error NAME FILE EXPLANATION
    local name="$1" file="$2" why="$3" ok=1 out code pat expected="minic exits 1 with:"
    out="$(cd "$(dirname "$file")" && "$MINIC" "$(basename "$file")" 2>&1 >/dev/null)"
    code=$?
    [ "$code" = 1 ] || ok=0
    while IFS= read -r pat; do
        expected="$expected '$pat'"
        grep -qF -- "$pat" <<<"$out" || ok=0
    done < <(directive "$file" EXPECT-ERROR)
    # Every message must use the file:line:col: error: format.
    grep -qvE '^[^:]+:[0-9]+:[0-9]+: error: ' <<<"$out" && ok=0
    if [ $ok = 1 ]; then report PASS "$name" "$why" "$expected" "exit=$code; $(flat "$out")"; else report FAIL "$name" "$why" "$expected" "exit=$code; $(flat "$out")"; fi
}

echo "== minic test cases =="
for file in "$HERE"/cases/*.c; do
    name="$(basename "$file" .c)"
    why="$(directive "$file" TEST | head -1)"
    if grep -q '^// EXPECT-ERROR:' "$file"; then
        run_error "$name" "$file" "$why"
    else
        run_valid "$name" "$file" "$why"
    fi
done

echo
echo "== examples (minic -O0 vs minic -O1 vs gcc) =="
for file in "$EXAMPLES"/*.c; do
    name="example:$(basename "$file")"
    case "$(basename "$file")" in
    err_*)
        if "$MINIC" "$file" >/dev/null 2>"$WORK/ex.err"; then
            report FAIL "$name" "error example must be rejected" "minic exits 1" "minic accepted it"
        else
            report PASS "$name" "error example must be rejected" "minic exits 1" "$(flat "$(cat "$WORK/ex.err")")"
        fi
        ;;
    *) run_valid "$name" "$file" "example program behaves the same everywhere" ;;
    esac
done

echo
echo "minic tests: $pass passed, $fail failed"
[ "$fail" = 0 ]
