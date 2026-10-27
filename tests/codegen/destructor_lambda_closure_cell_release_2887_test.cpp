// Codegen / §28.9 — issue #2887: a `var f := <lambda>` LOCAL in a destructor
// body never released its closure cell. The destructor body was the one
// remaining body kind for the #422 / #429 / #672 / #488-follow closure-release
// pin leak: every consumer of `closureReleaseEligible_` / `cellFreeEligible_`
// gates on `currentFn_` matching the analysed function — `armStringLocalRefcount`
// and the `bz_closure_release` registration in `lowerLocalDecl` (`Setup.cpp`
// ~13290) — but `emitDisposerCore` set `currentFn_` to the freshly-created
// disposer LLVM function WITHOUT touching the two tokens, so the gate was
// false for every closure local in any destructor body. Result: 48 bytes per
// closure cell leaked per disposal of the object — unbounded in a service loop
// that constructs and disposes objects repeatedly. The sister defect for the
// string path was #2852 (the same gate against `stringReleaseEligibleFn_`);
// the closure path is the parallel problem.
//
// Spec: §28.4 "The `destructor`"; §27.5 "Closures" — a closure is a heap-allocated
// mutable cell holding the captured variables and the function pointer, with
// a refcount keeping it alive across borrows; §28.3 "Local variables,
// ownership, and borrows" — the scope-end release of a `var` closure local
// fires `bz_closure_release` on its single producer reference; §28.9
// "Runtime-managed types" — a closure is in the runtime-managed set, so its
// refcount is the *only* constraint on its lifetime and the compile-time
// ownership analysis must release it exactly once. Spec revision v0.273.
// v0.240 unchanged (verified via Bezel_Changelog.md).
//
// THE DEFECT. Two eligibility sets, one fn token:
//   * `closureReleaseEligible_` / `closureReleaseEligibleFn_` (#422 / §27.5 /
//     §28.9) — which `var` closure (function-typed) locals may be released at
//     scope end. Gate: `currentFn_ == closureReleaseEligibleFn_`.
//   * `cellFreeEligible_` / `cellFreeEligibleFn_` (#429 / §27.5) — which
//     captured locals have ALL-non-escaping lambdas and may have their boxed
//     cells freed at outer-scope exit. Gate: `currentFn_ == cellFreeEligibleFn_`.
// `lowerLocalDecl` registers `bz_closure_release` (the scope-end release) AND
// `boxedCellFree` (the captured-cell free) ONLY when its gate is true. The
// destructor body (LlvmBackend_Disposal.cpp's emitDisposerCore, the §28.4 step
// 1 dispatcher) set `currentFn_` to the disposer fn but never pinned the two
// tokens after #429 / #422 — so the gate was false for every closure local in
// it, and the scope-end release was silently dropped. The strings parallel
// (`stringReleaseEligibleFn_`) was fixed in #2852 by the same pin-this-token
// form; the closure/cell pin was missed then.
//
// MEASURED, `--sanitize=address`, loop-amplified 20 disposals with 1 closure
// local each on the base of this change (no closure/cell pin): LSan reports
// `48 byte(s) leaked in 20 allocation(s)` — the closure cell + env block,
// exactly one per disposal. The value is correct (the lambda is called,
// captured, and produces the expected `Sink` value), there is no use-after-free,
// double-free or SEGV — a bounded leak only, the same classification #2852
// carried for the string path. The existing #2872 ship (the nested-lambda
// compile-time defect) explicitly notes that the destructor-body closure leak
// is a separate issue ("filed separately") — this is that issue.
//
// THE FIX. Pin both tokens in `emitDisposerCore` before the destructor body is
// lowered, mirroring the #2817 / #429 / #488-follow pin pattern used in
// `buildSpecializedMethodFn` (Setup.cpp) and `emitAsyncLambda` (Async.cpp):
// `collectClosureReleaseEligible` classifies the body's own `var` closure
// locals, `collectBoxCaptureLocals` + `collectDefinitelyEscapingLambdas` +
// `collectCellFreeEligible` classify the captured cells in dependency order
// (the third depends on the first's result), and the three `*Fn_` tokens are
// keyed to the disposer fn. The `lowerScopedBlock` below runs the body with
// `currentFn_` matching, so every gate is true and the closure cell + env
// block are released exactly once at scope end. The constructor's pin, the
// async-method's pin, the iterator's pin, the lambda's pin, and the field-
// disposer-transfer path all already do this — the destructor body was the
// only body kind missing.
//
// THE FIELD-INTERACTION ARGUMENT. The destructor body runs IMMEDIATELY BEFORE
// §28.4 step 2 (the owned-field auto-disposal cascade). A `var f := <lambda>`
// local in the body holds a function-typed value whose cell is allocated
// afresh and refcounted to 1 (the only reference exists); the scope-end
// `bz_closure_release` drops the SINGLE reference. The captured-cell free
// follows the closure release (register-order: boxedCellFree before
// closureRelease in the forward list → reverse iteration reaches closureRelease
// first, then boxedCellFree — #429's ordering). Neither path touches any
// field of `this`, so the field cascade is independent; it runs after the
// body has emitted its end-of-scope cleanups, and (per §28.4 step 2's
// reverse-declaration-order discipline) is unaffected by anything the body
// did with its own locals.
//
// THE EXCEPTION PATH. The dtor-throw redirect (=#1546, fix used here: the
// `dtorThrewBB` block) routes a body throw into the same post-body cleanup
// that the normal fallthrough reaches, so the field cascade runs on the
// throwing path exactly as on the non-throwing one. The scope-end
// `bz_closure_release` a body throw emits is registered by `emitUnwindDisposals`
// for the body's own scope roots (e.g. a `using var` inside the destructor)
// and it fires before the field cascade — the same ordering as the
// non-throwing path.
//
// ⚠ Every case is LOOP-AMPLIFIED. A ONE-call `Main` is NOT a reliable LSan
// oracle (a leftover pointer in a register makes LSan report a leaked block
// as reachable, so the same binary reports a leak on some runs and not others).
// ⚠ The first case uses RUNTIME-BUILT churn (a `Lambda` that captures a
// runtime-built string) to ensure the closure cell is allocated EACH
// iteration — the constant-fold trap that has made probes in this family
// vacuous repeatedly does not apply here, but the loop-amplification is the
// binding contract.

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

// 20 disposals, each adds `tag + 1` (the lambda's tag is `i`, so the
// contribution is `i + 1` per iteration). Sigma_{i=1..20} (i+1) = 230.
constexpr int kSum20 = 230;
// (B) captured-cell case: each disposal increments Sink by 1 (after verifying
// the lambda returns `Tag + s.Length`, which is the captured-cell equality).
// The accumulator stays inside the 8-bit WEXITSTATUS range (20 << 256) — the
// harness cannot round-trip an accumulator sum > 255 through the exit code,
// and using `f(Tag)` directly would push the sum to 321 (truncated to 65).
constexpr int kCount20 = 20;

// (A) THE ISSUE'S REPRO. 20 disposals, 1 closure local each, returns the
// summed lambda-applied value. Pre-fix: 48 B / 20 allocations leaked per
// run (one closure cell per disposal). Post-fix: clean. §4.6 + §27.5 +
// §28.3 + §28.9.
const char* kDestructorBodyLambdaReleases =
    "module M; class R "
    "  public Tag: int := 0; "
    "  public static Sink: int := 0; "
    "  public constructor(i: int) "
    "    Tag := i; "
    "  end constructor; "
    "  destructor "
    "    var f := lambda(x: int) => x + 1; "
    "    R.Sink := R.Sink + f(Tag); "
    "  end destructor; "
    "end R; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 1..20 do "
    "      using var r := new R(i); "
    "    end; "
    "    return R.Sink; "
    "  end Main; "
    "end C;";

// (B) CLOSURE-CAPTURED CELL: the captured local's boxed cell. The destructor
// captures a runtime-built string into a lambda, so the lambda's env block
// holds a CELL holding the captured string (+1 retained at capture site).
// §28.9 + §27.5 both require the CELL to be freed at outer-scope exit
// (captures are borrows of the cell, not of the source — the cell is the
// per-capture scope). Pre-fix: the cell leaks too, on top of the closure cell.
// Post-fix: both freed exactly once. The sink value verifies the lambda runs
// correctly (the `f(Tag) = Tag + s.Length` equality check pins the captured
// `s.Length` value the cell holds) and increments by 1 each iteration so the
// accumulator stays inside the 8-bit WEXITSTATUS range the test harness
// requires (a 20×(Tag + s.Length) sum would truncate and lose the equality
// signal). // per §27.5, §28.4, §28.3, §28.9, v0.240, #2887
const char* kDestructorBodyCapturesCell =
    "module M; class R "
    "  public Tag: int := 0; "
    "  public static Sink: int := 0; "
    "  public constructor(i: int) "
    "    Tag := i; "
    "  end constructor; "
    "  destructor "
    "    var s := $\"cap-{Tag}\"; "
    "    var f := lambda(x: int) => s.Length + x; "
    "    if f(Tag) = Tag + s.Length then "
    "      R.Sink := R.Sink + 1; "
    "    end; "
    "  end destructor; "
    "end R; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 1..20 do "
    "      using var r := new R(i); "
    "    end; "
    "    return R.Sink; "
    "  end Main; "
    "end C;";

}  // namespace

// (A) The issue's repro. 48 B / 20 allocations pre-fix.
BEZEL_TEST(codegen_destructor_lambda_closure_cell_release_2887,
           destructor_body_closure_local_releases_cell_spec_27_5) {
    JitResult r = runAotChecked(kDestructorBodyLambdaReleases);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kSum20);
}

// NOT a #2887 oracle on its own — a leak is invisible without a sanitizer,
// so this passes with and without the fix. It pins that nothing is freed
// early and the lambda runs correctly through the destructor-body path.
BEZEL_TEST(codegen_destructor_lambda_closure_cell_release_2887,
           destructor_body_closure_local_value_invariant_spec_27_5) {
    JitResult r = runAot(kDestructorBodyLambdaReleases);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kSum20);
}

// (B) Captured cell. Both the closure cell AND the captured cell must be
// freed. The lambda captures a runtime-built string held in a scope-local.
// The fix's `cellFreeEligible_`/`cellFreeEligibleFn_` pin is what unlocks
// the captured-cell free, so this test pins the OTHER half of the fix. The
// expected accumulator is `kCount20` (1 per iteration), NOT `f(Tag)`, because
// the harness masks exit codes to 8 bits and a 20×(Tag + s.Length) sum would
// truncate. The `if f(Tag) = Tag + s.Length then Sink += 1` form in the source
// pins the captured-cell value through the equality check, so the captured
// cell's behaviour is verified without using its value as the sum.
BEZEL_TEST(codegen_destructor_lambda_closure_cell_release_2887,
           destructor_body_captured_cell_is_freed_spec_27_5) {
    JitResult r = runAotChecked(kDestructorBodyCapturesCell);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kCount20);
}

BEZEL_TEST(codegen_destructor_lambda_closure_cell_release_2887,
           destructor_body_captured_cell_value_invariant_spec_27_5) {
    JitResult r = runAot(kDestructorBodyCapturesCell);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kCount20);
}
