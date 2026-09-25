#!/bin/bash
# usage: run_emitted.sh <probe.lua> [extra Fission.CLI flags, e.g. --optimize-ir]
# env: DBG="0 2" (debug levels, default 2), BUILD (default cmake-build-fuzz-current)
# Decompiles through Fission.CLI --decompile-test (AutoNameVariables + extras) at O0/O1/O2, executes the
# emitted source in the Fission.Fuzzing VM harness, and diffs its trace against the original source's.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/cmake-build-fuzz-current}
OUT=${TMPDIR:-/tmp}/fission-run-emitted
mkdir -p "$OUT"
src=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
b=$(basename "$src" .lua)
for o in 0 1 2; do
  for d in ${DBG:-2}; do
    "$BUILD/Fission.Fuzzing.exe" --sem-file "$src" --opt $o --debug $d > "$OUT/$b.orig.txt" 2>&1
    (cd "$OUT" && "$BUILD/Fission.CLI.exe" --decompile-test "$src" --opt $o --debug $d "$@" > "$OUT/$b.cli.txt" 2>&1)
    sed -n '/^===SOURCE===/,/^===END===/p' "$OUT/$b.cli.txt" | sed '1d;$d' > "$OUT/$b.O$o.d$d.out.lua"
    "$BUILD/Fission.Fuzzing.exe" --sem-file "$OUT/$b.O$o.d$d.out.lua" --opt $o --debug $d > "$OUT/$b.dec.txt" 2>&1
    t1=$(sed -n '/^---- original trace/,/^---- decompiled trace/p' "$OUT/$b.orig.txt")
    t2=$(sed -n '/^---- original trace/,/^---- decompiled trace/p' "$OUT/$b.dec.txt")
    if [ -z "$t2" ]; then echo "$b O$o d$d: NO-RUN $(head -1 "$OUT/$b.dec.txt")";
    elif [ "$t1" == "$t2" ]; then echo "$b O$o d$d: SAME";
    else echo "$b O$o d$d: DIFF"; diff <(echo "$t1") <(echo "$t2") | head -6 | cut -c1-240; fi
  done
done
