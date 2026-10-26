// Codegen / refcounting — issue #2852: the DESTRUCTOR body was the one remaining
// body kind for the #2807/#2817 `stringReleaseEligibleFn_` pin leak. Every
// `var` string local in a destructor body leaked both its allocations (the
// `bz_string` header plus its data buffer), per local, per disposal — unbounded
// in a service loop that constructs and disposes objects repeatedly.
//
// Spec: §4.6 "`string`"; §28.9 "Runtime-managed types" — "`string` — pervasive;
// created by literals, interpolation, concatenation, and library methods"; §28.3
// "Local variables, ownership, and borrows"; §28.4 "Destructors and disposal
// order" — the compiler-generated `Dispose()` "1. Runs the destructor body, if
// any. 2. Disposes owned fields ... 3. Chains to the base class's `Dispose()`."
// Spec revision v0.240. v0.270 unchanged (verified via Bezel_Changelog.md).
//
// THE DEFECT. Every consumer of the three string-eligibility sets gates on
// `currentFn_ == stringReleaseEligibleFn_`: armStringLocalRefcount (the sole
// registrar of the bind-time retain AND the scope-end Disposal{stringRelease}),
// returnedStringLocalOwnsPlusOne, the two record-backed interface-property
// retains, and the `:=` / §4.6 `+=` drop-on-overwrite release. After #2817 the
// token was pinned in eight places — emitFunctionBody, emitAsyncMethod's resume
// prolog, buildSpecializedMethodFn, emitIteratorMethod, emitAsyncIteratorMethod,
// lowerLambda, emitAsyncLambda, and the compiled-constructor body — but the
// destructor body (LlvmBackend_Disposal.cpp's emitDisposerCore, the §28.4 step
// 1 dispatcher) was never wired. The destructor body was lowered with
// `currentFn_` naming the freshly-created disposer LLVM function while the
// token still named some other body, so the gate was false for every string
// local in it.
//
// MEASURED, `--sanitize=address`, loop-amplified 20 disposals with 2 locals
// each, on the base of this change (no destructor pin): 2033 byte(s) leaked in
// 80 allocation(s). The value is correct and there is no use-after-free,
// double-free or SEGV — a bounded leak only, the same classification #2807/#2817
// carried for the other seven body kinds.
//
// THE FIX. Pin the token in `emitDisposerCore`, before the destructor body is
// lowered, mirroring the #2817 constructor pin: `collectStringReleaseEligible`
// classifies the body's own `var` string locals, then `stringReleaseEligibleFn_`
// is set to the disposer fn. The co-gated `retainAssignedStringParams` is
// called with an empty params vector — a destructor takes no parameters (§18),
// so this is a structural no-op, but the invariant is that the two predicates
// move together and the call makes the invariant checkable at review.
//
// THE FIELD-INTERACTION ARGUMENT. A destructor body runs IMMEDIATELY BEFORE
// §28.4 step 2 (the owned-field auto-disposal cascade), and a `var s := this.
// SomeStringField` borrow local inside it aliases a field the disposer is about
// to release. The #2817 borrow model retains at bind and releases at scope end,
// so the ORDER is: (a) field read at bind, (b) borrow local holds the reference,
// (c) scope-end release drops the borrow, (d) field cascade drops the
// original. retains >= releases on every path; the scope-end release
// (`Disposal{stringRelease}` emitted by armStringLocalRefcount for the local)
// fires BEFORE the field's release, since the field cascade runs after the
// body has emitted its end-of-scope cleanups. The string-local path is
// separate from the `armValueLikeChoiceStrRelease` `if (emittingDisposerBody_)
// return;` guard on §11 choice values (that walks fields directly, by §28.4's
// own design), and the analysis here is the correct one for the `var` string-
// local path.
//
// THE EXCEPTION PATH. The dtor-throw redirect (=#1546, LlvmBackend_Disposal.cpp
// lines 153-188) routes a body throw into the same `dtorThrewBB` cleanup that
// the normal fallthrough reaches, so the post-body field cascade runs on the
// throwing path exactly as on the non-throwing one. The scope-end
// Disposal{stringRelease} a body throw emits is registered by emitUnwindDisposals
// for the body's own scope roots (e.g. a `using var` inside the destructor) and
// it fires before the field cascade — the same ordering as the non-throwing path.
//
// ⚠ Every string here is built at RUNTIME (`$"dt-{Tag}"`): `"ab" + "cd"`
// constant-folds to an rc sentinel on which bz_string_release no-ops — the
// trap that has made probes in this family vacuous repeatedly.
// ⚠ Every case is LOOP-AMPLIFIED and allocates a churn string per iteration.
// A ONE-call `Main` is NOT a reliable LSan oracle (a leftover pointer in a
// register makes LSan report a leaked block as reachable, so the same binary
// reports a leak on some runs and not others).

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

// sum over i in 1..20 of len("tg-{i}") = 9*4 + 11*5 = 91.
constexpr int kLenSum20 = 91;

// (A) THE ISSUE'S REPRO. 20 disposals, 2 string locals each, runtime-built
// values to avoid the rc-sentinel constant-fold trap. Pre-fix: 2033 B / 80
// allocations. Post-fix: clean. §28.4 step 1 + §4.6 + §28.9.
const char* kDestructorBodyLocals =
    "module M; class R "
    "  public Tag: string := \"\"; "
    "  public Sink: int := 0; "
    "  public constructor(i: int) "
    "    Tag := $\"tg-{i}\"; "
    "  end constructor; "
    "  destructor "
    "    var built := $\"dt-{Tag}\"; "
    "    var churn := $\"churn-churn-churn-{Tag}-{Tag}\"; "
    "    if churn.Length < 0 then Sink := 1; end; "
    "    Sink := built.Length; "
    "  end destructor; "
    "end R; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var r := new R(i); "
    "      total := total + r.Tag.Length; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// (C) A SIMPLER HOLD-BORROW ORACLE — closer to the issue's structural shape:
// the destructor reads a field of `this`, captures it in a borrow local,
// builds a churn string, and the field itself is read again afterwards.
// Pre-fix: leak. Post-fix: exactly once. §28.3 / §28.4 / §28.9.
const char* kDestructorSelfFieldBorrow =
    "module M; class R "
    "  public Tag: string := \"\"; "
    "  public Sink: int := 0; "
    "  public constructor(i: int) "
    "    Tag := $\"tg-{i}\"; "
    "  end constructor; "
    "  destructor "
    "    var borrowed := Tag; "
    "    var churn := $\"churn-churn-churn-{borrowed}-{borrowed}\"; "
    "    if churn.Length < 0 then Sink := 1; end; "
    "    Sink := borrowed.Length; "
    "  end destructor; "
    "end R; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var r := new R(i); "
    "      total := total + r.Tag.Length; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

}  // namespace

// (A) The issue's repro. 2033 B / 80 allocations pre-fix.
BEZEL_TEST(codegen_destructor_body_string_local_release_2852,
           destructor_body_locals_are_released_spec_28_4) {
    JitResult r = runAotChecked(kDestructorBodyLocals);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_destructor_body_string_local_release_2852,
           destructor_body_plain_path_value_invariant_spec_28_4) {
    // NOT a #2852 oracle on its own — a leak is invisible without a sanitizer,
    // so this passes with and without the fix. It pins that nothing is freed
    // early.
    JitResult r = runAot(kDestructorBodyLocals);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// (C) Self-field borrow. Same shape as the issue but reads `this.Tag` instead
// of a `Holder` carried in. Exercise the field-interaction rule: the borrow
// local's scope-end release must drop the borrow, NOT the field's reference.
BEZEL_TEST(codegen_destructor_body_string_local_release_2852,
           destructor_borrow_local_field_survives_every_disposal_spec_28_3) {
    JitResult r = runAotChecked(kDestructorSelfFieldBorrow);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_destructor_body_string_local_release_2852,
           destructor_borrow_local_releases_exactly_once_spec_28_3) {
    JitResult r = runAot(kDestructorSelfFieldBorrow);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}
