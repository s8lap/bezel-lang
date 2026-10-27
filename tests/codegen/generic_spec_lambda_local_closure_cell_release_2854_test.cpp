// Codegen / §28.9 — issue #2854: a `var f := <lambda>` LOCAL in a generic-
// specialization body never released its closure cell. The specialization
// body was the one remaining body kind for the closure-release eligibility
// re-establishment: every consumer of `closureReleaseEligible_` gates on
// `currentFn_ == closureReleaseEligibleFn_` — the `bz_closure_release`
// registration in `lowerLocalDecl` (Setup.cpp ~13472) is the sole registrar
// for the scope-end release of the +1 producer reference — but
// `buildSpecializedMethodFn` set `currentFn_` to the freshly-created specFn
// WITHOUT pinning the token. The same gap was fixed for the string set by
// #2807 and for the unwind set by #1483; the closure set was the one that
// was missed on the specialization side. The non-generic twin of this
// program (a plain `Outer` method) is clean, which localizes the gap to the
// specialization path.
//
// Spec: §27.5 "Closures" — a closure is a heap-allocated mutable cell
// holding the captured variables and the function pointer, with a refcount
// keeping it alive across borrows; §28.3 "Local variables, ownership, and
// borrows" — the scope-end release of a `var` closure local fires
// `bz_closure_release` on its single producer reference; §28.9
// "Runtime-managed types" — a closure is in the runtime-managed set, so its
// refcount is the only constraint on its lifetime and the compile-time
// ownership analysis must release it exactly once; §14.1 "Generic
// declarations" for the monomorphized body. Spec revision v0.240.
//
// THE DEFECT. `lowerLocalDecl` registers `bz_closure_release` (the scope-end
// release of the +1 producer reference, #422 / #429) ONLY when
// `currentFn_ == closureReleaseEligibleFn_`. The token was assigned in
// exactly ONE other place in the backend (emitFunctionBody's `closure
// ReleaseEligible_.clear(); closureReleaseEligibleFn_ = fn;` prolog), while
// `buildSpecializedMethodFn` (the per-(class, type-arg) monomorphized body
// builder, LlvmBackend_Calls2.cpp ~11211) set `currentFn_ = specFn` but
// never touched the token. Every `var f := <lambda>` in a generic
// specialization body therefore lowered with `currentFn_` naming a fresh
// llvm::Function while the token still named the last synchronous method
// emitted, so the gate was false and `bz_closure_release` was silently
// dropped — exactly one closure cell per call leaked, per specialization,
// bounded but unbounded in a service loop. The sister defects the lane
// compared against:
//   * unwind (unwindIneligibleFn_, #1483) — already pinned in
//     buildSpecializedMethodFn (LlvmBackend_Calls2.cpp ~11531).
//   * strings (stringReleaseEligibleFn_, #2807) — already pinned since
//     #2807, also with the save/restore guard around the whole emission.
//   * capture cell (cellFreeEligibleFn_, #1416/#1418) and the
//     borrowing-collection sets (#2132/#2142/#2143) — NOT yet pinned here;
//     per the issue, the lane fix is the closure set only and the other
//     gaps are filed separately. A captured-local case (B) below is left out
//     intentionally so this test does not require the cell-free pin.
//
// MEASURED, `--sanitize=address`, loop-amplified 10 calls with one closure
// local each, on the base of this change (no closure pin): LSan reports
// `400 byte(s) leaked in 10 allocation(s)` — exactly the issue's
// measurement, one 40-byte closure cell per call. The value is correct
// (the lambda is called, computes the expected `inner.Length`, and
// contributes to the total), there is no use-after-free, no double-free,
// no SEGV — a bounded leak only, the same classification #2807 carried
// for the string set. The non-generic twin of this program is clean.
//
// THE FIX. Pin `closureReleaseEligible_` / `closureReleaseEligibleFn_` in
// `buildSpecializedMethodFn` alongside the #2807 string set pin, mirroring
// the same save/restore guard pattern: save at the start, `clear() + pin +
// collectClosureReleaseEligible(*ci.stmts)` after `bindParameters` for a
// `Block` body, restore at the bottom. The walk is a pure traversal of the
// shared body AST keyed on `const ast::LocalDecl*`, so a local whose
// declared type is the type PARAMETER (e.g. a `var f := lambda(): T => ...`)
// does not match the +1 closure-producer shape and is conservatively not
// admitted; a concretely function-typed local is classified identically in
// every instantiation. The save/restore is load-bearing: a lazy
// specialization is built MID-EMISSION of an outer body, and without the
// restore the outer's own `var f := <lambda>` (case (C) below) would
// silently lose its release once emission returned to it. The string pin
// verified this exact pattern; we do not measure the regressed state but
// the symmetry argues against omitting the restore.
//
// ⚠ Every case is LOOP-AMPLIFIED. A ONE-call `Main` is NOT a reliable LSan
// oracle (a leftover pointer in a register makes LSan report a leaked
// block as reachable, so the same binary reports a leak on some runs and
// not others). 10 calls x 1 closure local is the minimum that gives a
// deterministic LSan verdict in this defect class — same shape as the
// issue's repro, not derived from it.
// ⚠ The `before` / `after` / `churn` strings exist to keep the loop body
// non-trivial (the LSan false-positive in §28.9 programs is known to
// depend on a register-reachable leftover pointer; the constant-fold
// trap on `"ab" + "cd"` does not apply to `$"…"` so the string locals
// here always allocate). They are themselves clean (the string pin in
// #2807), so they contribute exactly zero to the leak and the 400 B
// measurement is purely the closure cell.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked - AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

std::string sanitizerText(const JitResult& r) {
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    return all;
}

// Issue's exact repro value: i in 1..10, each iteration contributes
// before.Length + mid + after.Length = (3 + len(i)) + (3 + len(i)) +
// (3 + len(i)) = 3 * (3 + len(i)) = 9 + 3*len(i). Sigma_{i=1..9} (9+3) +
// (9+6) = 9*12 + 15 = 108 + 15 = 123.
constexpr int kIssueReproValue = 123;

// A small churn allocation per iteration (the issue's own; non-escaping
// string, releases cleanly via the #2807 string pin — its only role here
// is to keep the loop body non-trivial so LSan doesn't report a false
// negative).
#define CHURN                                                                  \
    "    var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "                    \
    "    if churn.Length < 0 then return 99; end; "

// =====================================================================
// (A) THE ISSUE'S REPRO. 10 calls of `Box<int>.Run(i)`; each call binds
// one `var f := <lambda>` (no captures) and calls it. The closure cell
// allocated by the lambda producer must be released at scope end
// (one `bz_closure_release` per call). Pre-fix: 400 B / 10 allocations
// leaked (40-byte closure cell x 10). Post-fix: clean. §4.6 + §27.5 +
// §28.3 + §28.9 + §14.1.
// =====================================================================
const char* kGenericSpecLambdaLocal =
    "module M; class Box<T> "
    "  public static method Run(i: int): int "
    "    var before := $\"be-{i}\"; "
    "    var f := lambda(n: int): int => do "
    "      var inner := $\"in-{n}\"; "
    "      return inner.Length; "
    "    end; "
    "    var mid := f(i); "
    "    var after := $\"af-{i}\"; "
    CHURN
    "    return before.Length + mid + after.Length; "
    "  end Run; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + Box<int>.Run(i); "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// =====================================================================
// (B) THE SAVE/RESTORE GUARD. The specialization is built MID-EMISSION of
// an outer method that also has its own `var f := <lambda>` (one inner
// closure local declared BEFORE the triggering call, one declared AFTER).
// If `buildSpecializedMethodFn` left the closure sets / fn token pointing
// at the specialization, the outer's `after` (the lambda declared AFTER
// the spec was built) would silently lose its release once emission
// returned to it — `before` was already registered against the outer
// emitFunctionBody's own token before the spec was built, so it would
// still release; the clobber only affects closure locals registered
// AFTER the spec returns. So a broken save/restore shows up as one
// leaked cell per call (10 allocations x 40 B = 400 B / 10) — the
// outer's `after` alone. §14.1 / §28.9 / §28.3.
// =====================================================================
const char* kSpecBuiltMidOuterBodyWithClosure =
    "module M; class Box<T> "
    "  public static method Inner(i: int): int "
    "    var f := lambda(n: int): int => n + 1; "
    "    return f(i); "
    "  end Inner; "
    "end Box; "
    "class C "
    "  private static method Outer(i: int): int "
    "    var before := lambda(n: int): int => n; "
    "    var mid := Box<int>.Inner(i); "
    "    var after := lambda(n: int): int => n; "
    CHURN
    "    return before(i) + mid + after(i); "  // (i) + (i+1) + (i) = 3i+1
    "  end Outer; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + C.Outer(i); "
    "    end; "
    "    return total; "                 // sum_{i=1..10} (3i+1) = 3*55+10 = 175
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------
// (A) the issue's repro.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_generic_spec_lambda_local_closure_cell_release_2854,
           generic_spec_lambda_local_releases_closure_cell_spec_28_9) {
    JitResult r = runAotChecked(kGenericSpecLambdaLocal);
    BEZEL_EXPECT_TRUE(r.ran);            // 400 B / 10 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kIssueReproValue);
}

// NOT a #2854 oracle on its own — a leak is invisible without a sanitizer,
// so this passes with and without the fix. It pins that nothing is freed
// early and the lambda runs correctly through the specialization path.
BEZEL_TEST(codegen_generic_spec_lambda_local_closure_cell_release_2854,
           generic_spec_lambda_local_value_invariant_spec_28_9) {
    JitResult r = runAot(kGenericSpecLambdaLocal);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kIssueReproValue);
}

// ---------------------------------------------------------------------
// (B) save/restore guard. If the pin's save/restore were dropped, the
// outer's `after` closure local would lose its release once emission
// returned from the spec; `before` was already registered before the
// spec clobbered the token, so only `after` leaks. The run would still
// produce the same value (the closures' computed value doesn't depend on
// the release), but LSan would report one extra leaked cell per call
// (10 iterations x 40 B = 400 B / 10 allocations). Verified: the spec's
// own `Inner` body releases and the outer's `after` releases too.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_generic_spec_lambda_local_closure_cell_release_2854,
           outer_body_keeps_its_own_closure_release_state_spec_14_1) {
    JitResult r = runAotChecked(kSpecBuiltMidOuterBodyWithClosure);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 175);
}

BEZEL_TEST(codegen_generic_spec_lambda_local_closure_cell_release_2854,
           outer_body_closure_state_value_invariant_spec_14_1) {
    JitResult r = runAot(kSpecBuiltMidOuterBodyWithClosure);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 175);
}
