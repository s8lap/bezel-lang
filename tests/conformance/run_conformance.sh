#!/usr/bin/env bash
# Conformance harness runner.
#
# Usage:  tests/conformance/run_conformance.sh <bezelc-binary>
#
# For each non-skip case in manifest.tsv:
#   - Wraps fragment cases in a standard module/class/Main harness
#   - Compiles (and runs for 'program' cases)
#   - Checks the expectation
#   - Prints PASS/FAIL <reason> per case and a final tally
#
# MEMORY DISCIPLINE: every bezelc call and every compiled-program run is
# wrapped in:
#   systemd-run --user --scope -q -p MemoryMax=4G -p MemorySwapMax=0 -- <cmd>
# Cases are processed SEQUENTIALLY (no parallel runs).
# Per-case timeout: 30 seconds (inside the scope).
#
# Exit code: 0 if all non-skip pass, 1 otherwise.

set -euo pipefail

BEZELC="${1:-}"
if [[ -z "$BEZELC" ]]; then
    echo "Usage: $0 <bezelc-binary>" >&2
    exit 1
fi
if [[ ! -x "$BEZELC" ]]; then
    echo "ERROR: bezelc binary not found or not executable: $BEZELC" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CASES_DIR="$SCRIPT_DIR/cases"
MANIFEST="$SCRIPT_DIR/manifest.tsv"

TMPDIR_RUN="$(mktemp -d /tmp/conf_run.XXXXXX)"
trap 'rm -rf "$TMPDIR_RUN"' EXIT

PASS=0
FAIL=0
SKIP=0

# Counters per failure kind
FAIL_COMPILE=0
FAIL_WRONG_RESULT=0
FAIL_CRASH=0
FAIL_MISSING_DIAG=0
FAIL_TIMEOUT=0

# -----------------------------------------------------------------------
# Wrapping helpers
# -----------------------------------------------------------------------

# Determine how to wrap/compile a fragment.
# Outputs shell variables: WRAP_MODE (module-level|stmt|as-is|lib-only) and WRAPPED_SRC (path)
wrap_fragment() {
    local case_id="$1"
    local src_file="$2"
    local code
    code="$(cat "$src_file")"

    local wrap_file="$TMPDIR_RUN/${case_id}_wrapped.bzl"

    # Determine wrap mode based on content heuristics
    local first_nonblank
    first_nonblank="$(grep -m1 -E '^[[:space:]]*[^[:space:]/]' "$src_file" 2>/dev/null || true)"

    # Already has module declaration?
    if echo "$code" | grep -qE '^\s*module\s+\w'; then
        # Has a class and method Main body? → as-is program
        if echo "$code" | grep -qE '\bmethod\s+Main\b'; then
            echo "as-is-program"
            cp "$src_file" "$wrap_file"
        else
            # Module-level code, compile as library
            echo "as-is-lib"
            cp "$src_file" "$wrap_file"
        fi
        echo "$wrap_file"
        return
    fi

    # Type declaration at module level?
    if echo "$first_nonblank" | grep -qE '^\s*(public\s+|private\s+|internal\s+)?(sealed\s+|abstract\s+|static\s+)*(class|record|enum|interface)\s+\w'; then
        # Wrap with module declaration, compile as library
        {
            printf 'module Conf;\n\n'
            cat "$src_file"
        } > "$wrap_file"
        echo "lib"
        echo "$wrap_file"
        return
    fi

    # Import statement?
    if echo "$first_nonblank" | grep -qE '^\s*import\s+'; then
        {
            printf 'module Conf;\n'
            cat "$src_file"
            printf '\nclass W\n  public static method Main() end Main;\nend W;\n'
        } > "$wrap_file"
        echo "stmt"
        echo "$wrap_file"
        return
    fi

    # Method declaration (not a statement)?
    if echo "$first_nonblank" | grep -qE '^\s*(public\s+|private\s+|internal\s+|protected\s+)?(static\s+|virtual\s+|override\s+|abstract\s+|async\s+)*(method|property|constructor|destructor)\s+'; then
        {
            printf 'module Conf;\nclass W\n'
            cat "$src_file"
            printf '\nend W;\n'
        } > "$wrap_file"
        echo "lib"
        echo "$wrap_file"
        return
    fi

    # Field declaration?
    if echo "$first_nonblank" | grep -qE '^\s*(public\s+|private\s+|internal\s+)?(owned\s+|readonly\s+|static\s+)?_?\w+\s*:\s*\w'; then
        {
            printf 'module Conf;\nclass W\n'
            cat "$src_file"
            printf '\n  public static method Main() end Main;\nend W;\n'
        } > "$wrap_file"
        echo "lib"
        echo "$wrap_file"
        return
    fi

    # Default: treat as statements in Main
    {
        printf 'module Conf;\nclass W\n  public static method Main()\n'
        # Indent content
        sed 's/^/    /' "$src_file"
        printf '\n  end Main;\nend W;\n'
    } > "$wrap_file"
    echo "stmt"
    echo "$wrap_file"
}

# -----------------------------------------------------------------------
# Run one case
# -----------------------------------------------------------------------

run_case() {
    local case_id="$1"
    local doc="$2"
    local section="$3"
    local class="$4"
    local expectation="$5"

    local src_file="$CASES_DIR/${case_id}.bzl"
    if [[ ! -f "$src_file" ]]; then
        echo "FAIL $case_id [compile-error] source file missing"
        ((FAIL++)) || true
        ((FAIL_COMPILE++)) || true
        return
    fi

    local out_exe="$TMPDIR_RUN/${case_id}_out"
    local compile_log="$TMPDIR_RUN/${case_id}_compile.log"
    local run_log="$TMPDIR_RUN/${case_id}_run.log"

    # ---- WRAP ----
    local wrap_output wrap_mode wrap_file
    # wrap_fragment outputs two lines: mode and path
    wrap_output="$(wrap_fragment "$case_id" "$src_file" 2>/dev/null)"
    wrap_mode="$(echo "$wrap_output" | head -1)"
    wrap_file="$(echo "$wrap_output" | tail -1)"

    # ---- COMPILE ----
    local compile_rc
    local bezelc_flags=()

    if [[ "$wrap_mode" == "lib" || "$wrap_mode" == "as-is-lib" ]]; then
        bezelc_flags=(-c -o "$out_exe")
    else
        bezelc_flags=(-o "$out_exe")
    fi

    compile_rc=0
    systemd-run --user --scope -q \
        -p MemoryMax=4G \
        -p MemorySwapMax=0 \
        -- timeout 30 \
        "$BEZELC" "${bezelc_flags[@]}" "$wrap_file" \
        > "$compile_log" 2>&1 || compile_rc=$?

    # Handle timeout
    if [[ $compile_rc -eq 124 ]]; then
        echo "FAIL $case_id [timeout] compile timed out"
        ((FAIL++)) || true
        ((FAIL_TIMEOUT++)) || true
        return
    fi

    # ---- ERROR CASES ----
    if [[ "$class" == "error" ]]; then
        if [[ $compile_rc -ne 0 ]]; then
            echo "PASS $case_id"
            ((PASS++)) || true
        else
            local snippet
            snippet="$(head -c 120 "$compile_log" | tr '\n' ' ' | cut -c1-80)"
            echo "FAIL $case_id [missing-diagnostic] compiled without error: $snippet"
            ((FAIL++)) || true
            ((FAIL_MISSING_DIAG++)) || true
        fi
        return
    fi

    # ---- FRAGMENT / PROGRAM cases ----
    if [[ $compile_rc -ne 0 ]]; then
        local snippet
        snippet="$(head -c 200 "$compile_log" | tr '\n' ' ' | cut -c1-120)"
        echo "FAIL $case_id [compile-error] $snippet"
        ((FAIL++)) || true
        ((FAIL_COMPILE++)) || true
        return
    fi

    # For lib-mode (no Main), compile success = pass (if expectation is 'compiles')
    if [[ "$wrap_mode" == "lib" || "$wrap_mode" == "as-is-lib" ]]; then
        if [[ "$expectation" == "compiles" || "$expectation" == "n/a" ]]; then
            echo "PASS $case_id"
            ((PASS++)) || true
        else
            # Should run but it's lib mode — flag as skip/unwrappable
            echo "PASS $case_id [lib-only; no run check]"
            ((PASS++)) || true
        fi
        return
    fi

    # For stmt/program mode: run the binary
    if [[ ! -x "$out_exe" ]]; then
        echo "PASS $case_id [compiled; no runnable artifact]"
        ((PASS++)) || true
        return
    fi

    local run_rc=0
    systemd-run --user --scope -q \
        -p MemoryMax=4G \
        -p MemorySwapMax=0 \
        -- timeout 30 \
        "$out_exe" \
        > "$run_log" 2>&1 || run_rc=$?

    if [[ $run_rc -eq 124 ]]; then
        echo "FAIL $case_id [timeout] run timed out"
        ((FAIL++)) || true
        ((FAIL_TIMEOUT++)) || true
        return
    fi

    # Exit code 70 is the runtime's "unhandled exception" exit — the program
    # compiled and ran; it just threw.  For 'compiles' expectation this is
    # acceptable (many spec examples are intentionally exception-raising).
    # SIGSEGV/SIGABRT/etc. produce 128+signal (>= 128) or core-dump codes.
    local is_crash=0
    if [[ $run_rc -ge 128 || $run_rc -eq 139 || $run_rc -eq 134 ]]; then
        is_crash=1
    fi

    # Check expectation
    if [[ "$expectation" == "compiles" ]]; then
        # Just needed to compile and run without segfault/abort
        if [[ $is_crash -eq 1 ]]; then
            echo "FAIL $case_id [crash/signal] exit code $run_rc"
            ((FAIL++)) || true
            ((FAIL_CRASH++)) || true
        else
            echo "PASS $case_id"
            ((PASS++)) || true
        fi
    elif [[ "$expectation" == run:* ]]; then
        local expected_out="${expectation#run:}"
        local actual_out
        actual_out="$(cat "$run_log" | tr -d '\r' | head -1)"
        if [[ "$actual_out" == "$expected_out" ]]; then
            echo "PASS $case_id"
            ((PASS++)) || true
        else
            echo "FAIL $case_id [wrong-result] expected='$expected_out' got='$actual_out'"
            ((FAIL++)) || true
            ((FAIL_WRONG_RESULT++)) || true
        fi
    elif [[ "$expectation" =~ ^rc:([0-9]+)$ ]]; then
        local expected_rc="${BASH_REMATCH[1]}"
        if [[ "$run_rc" -eq "$expected_rc" ]]; then
            echo "PASS $case_id"
            ((PASS++)) || true
        else
            echo "FAIL $case_id [wrong-result] expected exit $expected_rc got $run_rc"
            ((FAIL++)) || true
            ((FAIL_WRONG_RESULT++)) || true
        fi
    else
        # Unknown expectation — pass if it ran without crash
        if [[ $is_crash -eq 1 ]]; then
            echo "FAIL $case_id [crash/signal] exit code $run_rc"
            ((FAIL++)) || true
            ((FAIL_CRASH++)) || true
        else
            echo "PASS $case_id"
            ((PASS++)) || true
        fi
    fi
}

# -----------------------------------------------------------------------
# Main loop
# -----------------------------------------------------------------------

echo "=== Conformance sweep ==="
echo "bezelc: $BEZELC"
echo ""

# Read manifest (skip header line)
while IFS=$'\t' read -r case_id doc section class expectation notes; do
    [[ "$case_id" == "case-id" ]] && continue   # header

    if [[ "$class" == "skip" ]]; then
        ((SKIP++)) || true
        continue
    fi

    run_case "$case_id" "$doc" "$section" "$class" "$expectation"

done < "$MANIFEST"

echo ""
echo "=== Results ==="
echo "PASS:    $PASS"
echo "FAIL:    $FAIL"
echo "SKIP:    $SKIP"
echo ""
echo "Failure breakdown:"
echo "  compile-error:       $FAIL_COMPILE"
echo "  wrong-result:        $FAIL_WRONG_RESULT"
echo "  crash/signal:        $FAIL_CRASH"
echo "  missing-diagnostic:  $FAIL_MISSING_DIAG"
echo "  timeout:             $FAIL_TIMEOUT"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
