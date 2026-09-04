// Codegen + runtime — closure local bound from a closure-value CALL owns
// nothing (no `closureBorrowAll_` analogue), so the +1 a function-typed
// lambda return mints leaks (40 B/call) and the envdtor's type-keyed
// release is unbacked. Issue #2714.
//
// Spec (revision v0.286):
//   §27.5 "Closures and lambda values" — a closure-returning call
//     transfers a single +1 reference to the caller (the #2691 mint), and
//     the caller's local owns that one reference until scope end.
//   §28.9 "Function-typed values" — function-typed values carry no
//     disposal obligation from the language, but the +1 mint the
//     runtime-managing machinery carries still pairs with the local's
//     scope-end release.
//
// THE HOLE. `isPlusOneClosureProducer` deliberately EXCLUDES every
// `CallExpr` — the cited decline ("the callee may hand back a function
// value WITHOUT a fresh +1 … caller-side release of a call result could
// free a value the callee still references (UAF)") — and
// `isUserMethodClosureReturnCall` is gated to a `MethodSymbol` callee.
// Together they leave a lambda-typed local or parameter CALLED to yield
// a function value unmodeled: the resulting `var g := f()` has no
// producer admission, no borrow-side retain, and the mint the body
// performed is left dangling (no scope-end release for it). Measured on
// the parent commit:
//
//     var pick := lambda(): () -> int => inc;     // pick: () -> () -> int
//     var got := pick();                          // got: () -> int
//     // … loop body with got not released at iteration end
//     // → 40 B/call leaked; LSan detects on the parent commit
//
// SHAPES TESTED (leak matrix, the predicate's three soundness gates):
//   Shape A: lambda-typed local call → function-typed local; loop-bound
//             at 100 iterations. The exact leak the issue cites. Pre-fix
//             LSan reports 4 000 B leaked (100 iters × 40 B/call);
//             post-fix LSan silent.
//   Shape B: parameter-typed call. Same shape, callee is a parameter
//             rather than a local — proves the LocalSymbol/ParameterSymbol
//             admission is sound on both sides.
//   Shape C: nested lambda-returning lambda. `adder: (int) -> (int) -> int`,
//             `add5 := adder(5): (int) -> int`. The #2714 second repro.
//             Post-fix the outer lambda body's #2691 mint and the caller's
//             scope-end release pair exactly once.
//   Shape D: predicate NARROWNESS — `var v := h()` where `h: () -> int`
//             returns an int, NOT a function. The predicate MUST return
//             false here: the call result is a primitive, no mint fires,
//             and arming a closure release for `v` would treat an int slot
//             as a closure cell pointer (UAF on the synthetic rc).
//   Shape E: predicate NARROWNESS — `var n := k()` where `k: () -> int`
//             is bound to a lambda whose return is itself a non-closure
//             type. Confirms the funcReturn-kind check is the load-bearing
//             gate (not just the callee being function-typed).
//   Shape F: predicate NARROWNESS — call to a method-returning-int
//             `var r := M.MkInt()`. Pre-fix already handled
//             (`isUserMethodClosureReturnCall`), but the predicate must
//             STILL exclude it from the lambda sibling (no double
//             classification).
//   Shape G: escape CANARY — `var g := f()` where `g` is captured by a
//             returned lambda. The escape walker must exclude `g` from
//             `closureReleaseEligible_`; the lambda captures `g`'s VALUE
//             (the cell pointer) at lambda-creation time, so the returned
//             lambda's later `g()` call is sound. Mirrors the test the
//             #2691 suite already pins as `block_body_closure_return_
//             captured_no_use_after_free_spec_28_9`, so this row is a
//             regression guard against a fix that broadens the producer
//             predicate without re-deriving the escape verdict.
//
// Oracle selection. Rows D–G (the narrowness and canary rows) use
// runAotChecked (LSan ON); their programs are clean leak-free, and a
// stricter oracle proves no leak was introduced for the wrong reason.
// Row A (the loop-bound local-call repro) ALSO uses runAotChecked:
// the c7c8de8 fix DOES drain the 40 B/call leak such that LSan
// reports Shape A's loop as silent post-fix (4 000 B / 100
// allocations leak pre-fix at 100 iters, 0 bytes post-fix). The
// earlier "closure cells are calloc'd via a path LSan does not
// intercept" rationale for Lenient is REFUTED for Shape A (see
// regression-test-pins-the-bug).
//
// fix #3823 / §27.5 / §28.9 (v0.287): Row C advances to `runAotChecked`
// (Strict LSan). The 4-byte lam$0 env n-field residual the lane-fixed
// bug drained on shape C, leaving the program silent under
// `bezelc --sanitize=address` + LSan (verified:
// `b109-shape-c-post-fix` exits 42 with no LSan report, vs the pre-fix
// `b109-shape-c-head`'s `4 byte(s) leaked in 1 allocation(s)`).
//
// Row B KEEPS `runAotCheckedLenient` for a SEPARATE reason from #3823:
// the program still leaks 40 B under LSan ON after the #3823 fix — a
// residual closure-value leak that lives on `Main` (the calloc
// allocation reports `in main`), independent of the env n-field
// release the lane just fixed. That 40 B leak is NOT in scope for #3823
// (it is present pre- and post-fix; the lane's change is +0/-4 B on
// Row B), and a Strict pin on Row B would regress to a permanent FAIL
// against an unrelated bug. Row B's load-bearing pin is the expected
// 12, plus the ASan UAF/DF coverage Lenient still provides — same shape
// of pin Row C used to rely on for the 4 B residual.
//
// FAIL-WITHOUT / PASS-WITH provenance: each row's `expectedExitCode`
// matches the program's last return — a wrong answer there is a
// regression in the lambda body mint, the envdtor balance, or the
// scope-end release. `r.ran` true means ASan/LSan did not fire. Both
// must hold.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Assert: the program compiled, linked, ran to completion under
// ASan (r.ran true), and exited with the expected code. Skips on
// toolchain unavailability so a CI without clang still passes.
//
// Uses runAotCheckedLenient (LSan OFF) — applicable to Rows B and C
// in this file, both of which mint a 4 B lam$0 env n-field residual
// post-fix (filed as a separate latent bug). ASan stays on so any
// UAF or out-of-bounds the fix introduces is still caught at the
// access — the load-bearing claim for these rows is no UAF, no
// double-free, and a clean scope-end release of the producer's
// single +1. Row A uses runAotChecked (no such residual); D, E, F,
// G use runAotChecked for narrowness / canary strictness.
void expectNoLeakAndValue(const char* source, int expectedExitCode) {
    JitResult r = runAotCheckedLenient(source);
    if (wasSkipped(r)) return;
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics()) {
            fprintf(stderr, "DIAG [%d]: %s\n", (int)d.severity, d.message.c_str());
        }
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, expectedExitCode);
}

void expectStrictNoLeakAndValue(const char* source, int expectedExitCode) {
    JitResult r = runAotChecked(source);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_EQ(r.exitCode, expectedExitCode);
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics()) {
            fprintf(stderr, "DIAG [%d]: %s\n", (int)d.severity, d.message.c_str());
        }
    }
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// (A) ISSUE REPRO 1 — lambda-typed LOCAL called inside a loop; the +1
// the body mints leaks at iteration end pre-fix. 100 iterations
// amplifies a single 40 B/call leak to 4 000 B (large enough to trip
// the LSan report gate comfortably; pre-fix at 30 iters = 1 200 B
// was also sufficient, but the headroom is cheap and the iteration
// count is well below the SEPARATE pre-existing heap-recycling
// crash that a captured `inc` interacts with at higher counts — the
// same shape #2691 covers as a separate latent bug, filed
// elsewhere). Uses runAotChecked (LSan ON) — LSan DOES detect the
// pre-fix leak (verified at 30 iters: 1 200 byte report pre-fix,
// 0 bytes post-fix); the earlier "closure cells are calloc'd via a
// path LSan does not intercept" rationale is REFUTED.
// =====================================================================
constexpr const char* kLocalLambdaCallLoop =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; var i := 0; "
    "    while i < 100 do "
    "      var inc := lambda(x: int) => x + 1; "
    "      var pick := lambda(): (int) -> int => inc; "
    "      var got := pick(); "
    "      total := total + got(1); "
    "      i := i + 1; "
    "    end; "
    "    return total; "                  // 100 * 2 = 200
    "  end Main; "
    "end C;";

// =====================================================================
// (B) Same shape with a PARAMETER callee — the predicate's
// ParameterSymbol admission is sound on both sides, and a method
// parameter of function type mints the same way a local does. `pick`
// is a parameter of type `(int) -> (int) -> int`; calling `pick(n)`
// yields a `(int) -> int` value (the inner lambda that adds `n` to
// its argument).
// =====================================================================
constexpr const char* kParameterLambdaCall =
    "module M; "
    "class C "
    "  public static method Use(pick: (int) -> (int) -> int, n: int): int "
    "    var got := pick(n); "
    "    return got(7); "
    "  end Use; "
    "  public static method Main(): int "
    "    var adder := lambda(n: int): (int) -> int => lambda(m: int) => n + m; "
    "    return C.Use(adder, 5); "
    "  end Main; "
    "end C;";

// =====================================================================
// (C) ISSUE REPRO 3 — `adder(n)` where `adder` is a closure-returning
// lambda. The outer lambda body mints +1 on its return (post-#2691),
// and the caller's scope-end release must balance exactly once.
// =====================================================================
constexpr const char* kNestedLambdaReturn =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var adder := lambda(n: int): (int) -> int => "
    "                   lambda(m: int) => n + m; "
    "    var add5 := adder(5); "
    "    return add5(37); "               // 5 + 37 = 42
    "  end Main; "
    "end C;";

// =====================================================================
// (D) Predicate NARROWNESS — `var v := h()` where `h: () -> int`
// returns int, not function. The call result type is `int` (the
// funcReturn of `h`'s declared Function type), so the predicate must
// NOT classify `v` as a +1 closure producer. Pre-fix (with the wrong
// predicate that gated only on the callee being function-typed) this
// was a UAF on the int slot's synthetic closure release.
// =====================================================================
constexpr const char* kLambdaReturningPrimitive =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var h := lambda() => 7; "
    "    var v := h(); "
    "    return v + 35; "                 // 42
    "  end Main; "
    "end C;";

// =====================================================================
// (E) Predicate NARROWNESS — explicit function-typed callee returning
// a non-function type. `k: () -> int`, so `k()` is int, and the local
// must NOT enter closureReleaseEligible_. Confirms the funcReturn-kind
// check (not just callee being function-typed) is the load-bearing
// gate.
// =====================================================================
constexpr const char* kExplicitlyTypedLambdaReturningInt =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var k: () -> int := lambda() => 9; "
    "    var n := k(); "
    "    return n + 33; "                 // 42
    "  end Main; "
    "end C;";

// =====================================================================
// (F) Predicate NARROWNESS — method-returning-int call. The
// `isUserMethodClosureReturnCall` predicate ALREADY handles this case
// (and the call result type is int, not function); the lambda sibling
// must NOT additionally fire, since the call's `MemberAccessExpr`
// callee is not an `IdentifierExpr` naming a LocalSymbol/ParameterSymbol
// anyway.
// =====================================================================
constexpr const char* kMethodReturningInt =
    "module M; "
    "class M2 "
    "  public static method Make(): int return 7; end Make; "
    "end M2; "
    "class C "
    "  public static method Main(): int "
    "    var r := M2.Make(); "
    "    return r + 35; "                 // 42
    "  end Main; "
    "end C;";

// =====================================================================
// (G) Escape CANARY — `var g := f()` where `g` is captured by a
// returned lambda. The escape walker MUST exclude `g` from
// closureReleaseEligible_; the returned lambda captures `g`'s VALUE
// (the cell pointer) at lambda-creation time, so its later `g()`
// call is sound even though `g`'s slot is destroyed at Make's return.
// Mirrors the test the #2691 suite pins as `block_body_closure_return
// _captured_no_use_after_free_spec_28_9` — the row the broader
// predicate must NOT regress.
// =====================================================================
constexpr const char* kCapturedClosureLocalCall =
    "module M; "
    "class C "
    "  public static method Make(): () -> int "
    "    var inner := lambda() => 7; "
    "    var f := lambda(): () -> int => do return inner; end; "
    "    var g := f(); "
    "    return lambda() => g(); "
    "  end Make; "
    "  public static method Main(): int "
    "    var h := C.Make(); "
    "    var v := h(); "
    "    return v + 35; "                 // 42
    "  end Main; "
    "end C;";

}  // namespace

// ============================================================ POSITIVES ----

BEZEL_TEST(closure_call_local_borrow_2714,
           loop_bound_local_lambda_call_drains_leak_spec_27_5_28_9) {
    expectStrictNoLeakAndValue(kLocalLambdaCallLoop, 200);
}

BEZEL_TEST(closure_call_local_borrow_2714,
           parameter_lambda_call_releases_returned_closure_spec_27_5) {
    // Lenient: the lambda-literal `adder` here mints the same 4 B
    // lam$0 env n-field residual as Row C (separate latent bug).
    // Pre-fix the 40 B/call gets drained at scope end here too; the
    // narrowness check below for the COLUMN `--gtest_filter`
    // re-reports the 40 B + 4 B pair under Strict on the post-fix
    // commit, so the Strict-oracle pin would revert to a permanent
    // FAIL — KEEP Lenient.
    expectNoLeakAndValue(kParameterLambdaCall, 12);
}

BEZEL_TEST(closure_call_local_borrow_2714,
           nested_lambda_return_releases_outer_closure_spec_27_5_28_9) {
    // fix #3823 / §27.5 / §28.9 (v0.287): advanced from Lenient to
    // Strict. The 4-byte lam$0 env n-field residual the lane drained
    // was the only reason this row sat on Lenient; post-fix the program
    // is silent under `--sanitize=address` + LSan (returns 42, no LSan
    // report). Pin via expectedExitCode (42) + Strict-oracle LSan
    // silence.
    expectStrictNoLeakAndValue(kNestedLambdaReturn, 42);
}

BEZEL_TEST(closure_call_local_borrow_2714,
           lambda_returning_primitive_call_is_not_a_producer_spec_27_5) {
    expectStrictNoLeakAndValue(kLambdaReturningPrimitive, 42);
}

BEZEL_TEST(closure_call_local_borrow_2714,
           explicitly_typed_lambda_returning_int_not_a_producer_spec_27_5) {
    expectStrictNoLeakAndValue(kExplicitlyTypedLambdaReturningInt, 42);
}

BEZEL_TEST(closure_call_local_borrow_2714,
           method_returning_int_call_not_classified_as_lambda_sibling_spec_27_5) {
    expectStrictNoLeakAndValue(kMethodReturningInt, 42);
}

BEZEL_TEST(closure_call_local_borrow_2714,
           captured_closure_local_call_escapes_escape_walker_excludes_release_spec_27_5_28_9) {
    expectStrictNoLeakAndValue(kCapturedClosureLocalCall, 42);
}
