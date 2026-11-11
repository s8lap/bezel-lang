#!/usr/bin/env bash
# Builds and runs the four runnable samples (Go, Rust, Java, Python) in every mode and
# writes RESULTS.txt: the first line of stdout, the message line of stderr (headers such as
# "Traceback" and "thread 'main' panicked at" are skipped) and the exit status.
# The C#, Swift and Kotlin samples are not built: no toolchain was on the research machine.
# Usage: run.sh [build-dir]     (default /var/tmp/bezel-scratch/fk/snip; never the repo)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-/var/tmp/bezel-scratch/fk/snip}"
mkdir -p "$out"
modes="user-file required-file telemetry primary-api user-number constant-number constant-number-wrong user-key own-key own-key-wrong"

(cd "$out" && cp "$here/demo.go" . && GOFLAGS=-mod=mod GO111MODULE=off go build -o demo_go demo.go) || echo "go build failed" >&2
rustc --edition 2021 -o "$out/demo_rs" "$here/demo.rs" 2>"$out/rustc.log" || echo "rustc failed" >&2
javac -d "$out" "$here/Demo.java" 2>"$out/javac.log" || echo "javac failed" >&2

run() {  # label, command...
  local label="$1"; shift
  for m in $modes; do
    "$@" "$m" >"$out/o.txt" 2>"$out/e.txt"; local rc=$?
    printf '%-7s %-22s exit=%-3s out=[%s] err=[%s]\n' "$label" "$m" "$rc" \
      "$(head -n1 "$out/o.txt")" "$(grep -v -E "^(Picked up|thread '|Traceback|note:|[[:space:]])" "$out/e.txt" | grep -m1 . | cut -c1-120)"
  done
}
{
  echo "# $(go version | cut -d' ' -f3), $(rustc --version | cut -d' ' -f1-2), $(javac -version 2>&1 | grep -v '^Picked up'), $(python3 --version)"
  run go     "$out/demo_go"
  run rust   "$out/demo_rs"
  run java   java -cp "$out" Demo
  run python python3 "$here/demo.py"
  echo "# javac CheckedForcesHandler.java (a caller with no handler for a checked exception):"
  javac -d "$out" "$here/CheckedForcesHandler.java" 2>&1 | grep -v '^Picked up' | grep -m1 'error:' | sed 's|.*/||'
} > "$here/RESULTS.txt"
cat "$here/RESULTS.txt"
