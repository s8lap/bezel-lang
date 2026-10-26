// Codegen / refcounting — a BLOCK-bodied lambda whose return type is a FUNCTION
// type did not mint the +1 its EXPRESSION-bodied sibling mints, so the same
// program was sound or a heap-use-after-free depending on nothing but which body
// form the author wrote. Issue #2691 — the closure-category sibling of #2688,
// which fixed the identical asymmetry for `string`.
//
// Spec: §27.3 "Lambda expressions" (the expression body and the block body are
// two spellings of ONE construct — the return-type ascription "behaves exactly
// as a method's declared return type", so both forms carry the same return
// contract), §27.5 "Closures" ("Captured variables are heap-allocated in a
// closure object"), §27.7 "Calling function values", §28.9 "Runtime-managed
// types" (a closure cell is refcounted: every co-owner takes exactly one
// reference and releases exactly one). Spec revision v0.240.
//
//     lambda(): () -> int => inner                 ->  +1  (sound)
//     lambda(): () -> int => do return inner; end  ->  +0  (BROKEN)
//
// THE BUG. `lowerLambda` (LlvmBackend_Exprs2.cpp) nulls `currentReturnTypeNode_`
// before emitting a lambda body — correct on its own terms (#2515: a lambda body
// must not box or mint against the ENCLOSING METHOD's return node) — but that
// also switched off `emitReturn`'s CLOSURE +1 mint, which keys on that node
// (LlvmBackend_Stmts.cpp). The expression-body path was unaffected: it keys off
// `retSemaTy`, the lambda's OWN return type. #2688 fixed exactly this for the
// `string` category and introduced `currentLambdaReturnSemaTy_` to carry the
// lambda's own return type into the return path; the closure category was left
// out, which is what #2691 reported.
//
// WHY IT IS FATAL, AND WHERE. Most consumers of a closure-value call result
// treat it as a +0 borrow — `isPlusOneClosureProducer` deliberately excludes
// every CallExpr, and `isUserMethodClosureReturnCall` requires a MethodSymbol
// callee, which a closure-value call never has. The one consumer that does NOT
// go through those predicates is the lambda ENVIRONMENT DESTRUCTOR: when the
// returned closure is bound to a local that is then CAPTURED, the capturing
// env's dtor releases that cell keyed purely on the capture's sema type
// (`ce.semaTy->kind == Kind::Function`, LlvmBackend_Exprs2.cpp), on the
// invariant that a closure local's slot owns +1. The expression form established
// that +1; the block form did not, so the block form over-released:
//   AddressSanitizer: heap-use-after-free ... in lam$3
//
// THE FIX. Mint in `emitReturn` against `currentLambdaReturnSemaTy_` when there
// is no declared return-type node, mirroring the expression-body closure
// predicate EXACTLY — the same three transfer exclusions, and, as there, no
// `!nullable` gate (the closure category has never had one, unlike the string
// category). Agreement between the two body forms is the invariant #2688
// established, and it is restored here at the sound value, +1.
//
// ⚠ THE PROGRAMS' SHAPE IS LOAD-BEARING. The returned closure must be a BORROW
// (a captured closure local), not a `lambda` expression: a lambda body is a +1
// producer, excluded by `isPlusOneClosureProducer` in both forms, so a probe
// returning a fresh `lambda(...)` observes nothing — case (E) pins that arm. And
// the result must be CAPTURED, not merely bound: a plain `var got := f();` bind
// has no consumer at all, so it cannot distinguish the forms by a crash.
//
// ⚠ KNOWN, ACCEPTED CONSEQUENCE (filed separately, NOT fixed here). A closure
// local initialized from a closure-value call is not retained at bind (there is
// no closure analogue of the string side's `stringBorrowAll_`) and is not
// registered for scope-end release, so when such a local is NOT captured the
// minted +1 has no consumer and leaks one 40-byte cell per call. Pre-fix that
// leak affected the expression form only; it now affects both forms equally,
// which is the point — the two spellings agree, and a bounded leak is the
// correct side to fail on versus a use-after-free.
//
// OBSERVABILITY. `runAotChecked` links and runs under ASan/LSan, so both failure
// directions flip `r.ran` to false: the use-after-free (too few mints — the bug)
// and a leak (too many — an over-correction). 200 iterations of allocation churn
// sit between the free and the read in (A)/(B), so a stale read cannot come back
// intact. The exit code independently pins the value computed through the
// returned closure, and (D) re-runs the repro on the PLAIN, non-sanitized path,
// because an ASan-only assertion can mask a live SIGSEGV (cf. #2633/#2629).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// (A) THE #2691 REPRO. `f`'s ascribed return type is the function type
// `() -> int` and its BLOCK body returns the captured closure local `inner` — a
// borrow, so none of the three transfer exclusions apply and the mint was owed.
// `g` binds that result and is then CAPTURED by the returned lambda, whose
// envdtor releases `g`'s cell (§27.5 / §28.9). Pre-fix `g` held +0, so that
// release freed `inner`'s cell while the returned closure still called through
// it: heap-use-after-free in `lam$3`.
// Value: h() + 35 = 7 + 35 = 42.
const char* kBlockBodyClosureReturnCaptured =
    "module M; class C "
    "  public static method Make(): () -> int "
    "    var inner := lambda() => 7; "
    "    var f := lambda(): () -> int => do return inner; end; "
    "    var g := f(); "
    "    return lambda() => g(); "
    "  end Make; "
    "  public static method Main(): int "
    "    var h := C.Make(); "
    "    var v := h(); "
    "    var churn := 0; "
    "    var i := 0; "
    "    while i < 200 do "                       // recycle any freed block
    "      var junk := lambda(x: int) => x + i; "
    "      churn := churn + junk(1); "
    "      i := i + 1; "
    "    end; "
    "    return v + 35; "                         // 42
    "  end Main; "
    "end C;";

// (B) The EXPRESSION-body spelling of the very same program. It was ALREADY
// sound — its mint pairs with the envdtor's release — and must stay so. This is
// the A/B pair that makes the defect a pure function of the body form, and the
// guard that the fix did not disturb the form that was already right. §27.3
// requires the two spellings to agree, and (A) and (B) now do: both 42.
const char* kExprBodyClosureReturnCaptured =
    "module M; class C "
    "  public static method Make(): () -> int "
    "    var inner := lambda() => 7; "
    "    var f := lambda(): () -> int => inner; "
    "    var g := f(); "
    "    return lambda() => g(); "
    "  end Make; "
    "  public static method Main(): int "
    "    var h := C.Make(); "
    "    var v := h(); "
    "    var churn := 0; "
    "    var i := 0; "
    "    while i < 200 do "
    "      var junk := lambda(x: int) => x + i; "
    "      churn := churn + junk(1); "
    "      i := i + 1; "
    "    end; "
    "    return v + 35; "                         // 42
    "  end Main; "
    "end C;";

// (C) The borrow ESCAPES its defining method by a plain method return rather
// than through a capture: `Make` returns `pick()`, and `emitReturn`'s ordinary
// closure branch (the declared-node one) mints its own +1 for it. Adding the
// lambda-level mint must not double-count that path into a leak, and must not
// leave it short into a UAF. Green before and after — an over-correction guard.
// Value: f() + 35 = 42.
const char* kBlockBodyClosureReturnEscapesByMethodReturn =
    "module M; class C "
    "  public static method Make(): () -> int "
    "    var inc := lambda() => 7; "
    "    var pick := lambda(): () -> int => do return inc; end; "
    "    return pick(); "
    "  end Make; "
    "  public static method Main(): int "
    "    var f := C.Make(); "
    "    var churn := 0; "
    "    var i := 0; "
    "    while i < 200 do "
    "      var junk := lambda(x: int) => x + i; "
    "      churn := churn + junk(1); "
    "      i := i + 1; "
    "    end; "
    "    return f() + 35; "                       // 42
    "  end Main; "
    "end C;";

// (E) The TRANSFER arm: the block body returns a FRESH `lambda` — a +1 producer
// that already owns its single reference — so the fix must TRANSFER it, exactly
// as the expression-body path does via `isPlusOneClosureProducer`. An
// unconditional mint would leave `g` at +2 against the envdtor's one release and
// LSan would fail this. Green before and after — the over-mint guard.
// Value: h() + 35 = 42.
const char* kBlockBodyProducerReturnTransfers =
    "module M; class C "
    "  public static method Make(): () -> int "
    "    var f := lambda(): () -> int => do return lambda() => 7; end; "
    "    var g := f(); "
    "    return lambda() => g(); "
    "  end Make; "
    "  public static method Main(): int "
    "    var h := C.Make(); "
    "    return h() + 35; "                       // 42
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #2691 — a BLOCK-bodied lambda's function-typed return now mints the +1 the
// capturing env's destructor releases. §27.3 / §27.5 / §28.9.
// FAILS without the fix: AddressSanitizer heap-use-after-free in `lam$3`.
// =====================================================================
BEZEL_TEST(codegen_lambda_closure_return_mint_agreement_2691,
           block_body_closure_return_captured_no_use_after_free_spec_28_9) {
    JitResult r = runAotChecked(kBlockBodyClosureReturnCaptured);
    BEZEL_EXPECT_TRUE(r.ran);         // no UAF (the bug) and no leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);  // the returned closure still computes 7
}

// =====================================================================
// (B) The EXPRESSION-body spelling of (A) was already sound and stays sound —
// §27.3's two body forms now agree, both 42.
// =====================================================================
BEZEL_TEST(codegen_lambda_closure_return_mint_agreement_2691,
           expr_body_closure_return_captured_still_sound_spec_27_3) {
    JitResult r = runAotChecked(kExprBodyClosureReturnCaptured);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// (C) A borrow escaping by an ordinary method return is neither double-counted
// nor left short by the new lambda-level mint. §27.5 / §27.7 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_closure_return_mint_agreement_2691,
           block_body_closure_return_escaping_by_method_return_balanced_spec_27_7) {
    JitResult r = runAotChecked(kBlockBodyClosureReturnEscapesByMethodReturn);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// (D) The repro on the PLAIN, non-sanitized AOT path — an ASan-only assertion
// can mask a live SIGSEGV in the ordinary build (#2633/#2629), so the value is
// pinned here independently of the sanitizer. §27.3 / §27.7.
// =====================================================================
BEZEL_TEST(codegen_lambda_closure_return_mint_agreement_2691,
           block_body_closure_return_captured_plain_path_value_spec_27_7) {
    JitResult r = runAot(kBlockBodyClosureReturnCaptured);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// (E) A +1 producer body is TRANSFERRED, not retained — pins the
// `isPlusOneClosureProducer` exclusion (an unconditional mint leaks here).
// §27.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_closure_return_mint_agreement_2691,
           block_body_producer_return_transfers_without_extra_mint_spec_28_9) {
    JitResult r = runAotChecked(kBlockBodyProducerReturnTransfers);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
