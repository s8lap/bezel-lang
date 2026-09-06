// Spec-derived tests for issue #3993 — `handleReturn` never verified that an
// `owned`-declared return type's return expression is actually an OWNED
// production. Returning a plain (non-`move`, non-owned-field-read) BORROW —
// a borrowed parameter, `this`, or a Borrowed-state local — through an
// `owned` return type compiled with ZERO diagnostics and double-freed at
// runtime: the caller's `owned` contract disposes the returned value, and
// the borrow's real owner disposes the same object again.
//
// Spec, v0.294, §28.3 "Borrow lifetime rules" (verbatim, first bullet):
//   "A borrowed local may not be returned from a method whose return type
//    is `owned`."
// §28.6 "Methods returning `this`" (verbatim):
//   "The returned borrow may not be assigned to an `owned` field or
//    returned as `owned` — it is the receiver under a different name."
//
// THE BUG (measured pre-fix on origin/main e240743, confirmed on this
// worktree by reverting the fix and rebuilding):
//
//   public static method Pick(a: Res, b: Res): owned Res return b; end Pick;
//
//   compiled with rc=0 and zero diagnostics (only the unrelated, expected
//   "`c` is implicitly disposed at scope end" auto-drop note at the call
//   site); running the compiled program under AddressSanitizer produced a
//   heap-use-after-free — `b`'s object is freed once via `c`'s scope-end
//   auto-drop (its tracked state is Owned, taken from the DECLARED `owned`
//   return type) and again via `b`'s own `using var` scope-end disposal.
//
// ROOT CAUSE: `MethodOwnership::handleReturn` (Ownership.cpp) runs a long
// list of ESCAPE checks (checkClosureEscapesOnReturn,
// checkRecordBorrowEscapesOnReturn, checkBorrowReturnedThroughOwningType,
// checkStaticBorrowReturn, checkInstanceBorrowReturn, checkOwnedFieldReturn,
// …) — every one of them rejects a borrow escaping through a NON-owning
// return type. None of them checked the opposite direction: a return
// expression that is NOT an owned production, reached through an `owned`
// return type. The two existing owned-return borrow-source checks were each
// scoped to one narrow syntactic shape — checkOwnedReturnBorrowSource to a
// `move E` operand (#1995), checkOwnedFieldReturn to a bare OWNED-field read
// (#1005/#2368) — and handleReturn's bare-identifier discharge fast-path
// (Slice O1) unconditionally marked a returned identifier "disposed" with no
// regard to the declared return type at all, so a bare `return b;` of a
// plain borrowed parameter slipped through untouched by any of them.
//
// THE FIX: `checkOwnedReturnDirectBorrow` (Ownership_Checks.cpp) — the
// direct-read sibling of checkOwnedReturnBorrowSource, run from handleReturn
// BEFORE the bare-identifier fast path (and from the `=>` expression-body
// sibling runExprBodyOwningReturnCheck) — feeds the exact §28.8
// `ownedArgSourceIsBorrow` classifier the sibling checks already use (rather
// than a second spelling, per this project's dominant-defect-generator
// rule): a no-op unless the declared return type is `owned`; excludes
// `move E` and a bare owned-field read (each diagnosed by its own existing,
// more specific check) so every borrow shape is reported exactly once.
//
// NOT fixed here (filed separately, #4004/#4005 — a structurally DIFFERENT
// defect: the surrounding driver never reaches handleReturn/
// runExprBodyOwningReturnCheck with the operator's own signature at all):
//   * a block- or expression-bodied `operator +` returning the identical
//     borrowed-parameter shape through an `owned` return type — measured
//     zero diagnostics + ASan heap-use-after-free, unaffected by this fix;
//   * a lambda checked against a `() -> owned T` function-type slot,
//     returning a captured borrow — same measured symptom, same non-fix.
//
// per §28.3 *Borrow lifetime rules*, §28.6 *Methods returning `this`*, v0.294

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot / runAotChecked — AOT(+ASan) oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The #3993 direct-borrow-return diagnostic's stable substring.
constexpr const char* kDirectBorrowReturn =
    "a borrow cannot be returned as an `owned` value";

// A destructor-counter Res: ctor ++, dtor --. A correct program's exit code
// (this file threads the counter out through the process exit code) is 0 —
// a double-free drives it negative or aborts under ASan before `Main`
// returns at all (the exact-once oracle).
const char* kRes =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// REJECT — a direct (non-`move`, non-owned-field-read) BORROW returned
// through an `owned` return type. Fail-without: accepted with zero
// diagnostics, double-freed at runtime (ASan heap-use-after-free, measured).
// Pass-with: rejected at compile time with a real source location.
// ===========================================================================

// The issue's exact repro: a STATIC method's plain borrowed parameter
// returned through `owned`, block body.
BEZEL_TEST(owned_return_direct_borrow_3993,
           static_method_borrowed_param_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Pick(a: Res, b: Res): owned Res return b; end Pick; "
        "  public static method Main(): int "
        "    using var a := new Res(3); using var b := new Res(4); "
        "    var c := Pick(a, b); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// The `=>` expression-body sibling of the same shape — runExprBodyOwningReturnCheck.
BEZEL_TEST(owned_return_direct_borrow_3993,
           static_method_exprbody_borrowed_param_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Pick(a: Res, b: Res): owned Res => b; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// The INSTANCE-method sibling: a plain parameter borrow returned through an
// `owned` return type is not receiver-sourced and must be rejected exactly
// as the static case is.
BEZEL_TEST(owned_return_direct_borrow_3993,
           instance_method_borrowed_param_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public method PickOther(other: Res): owned Res return other; end PickOther; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// §28.6 *Methods returning `this`*, verbatim: "may not be ... returned as
// `owned` — it is the receiver under a different name."
BEZEL_TEST(owned_return_direct_borrow_3993,
           instance_method_bare_this_rejected_spec_28_6) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public method Self(): owned Res return this; end Self; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// A local bound to a borrow (`var x := b;`) is itself Borrowed state —
// returning it through `owned` is the same hazard one level removed.
BEZEL_TEST(owned_return_direct_borrow_3993,
           borrowed_state_local_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Pick(a: Res, b: Res): owned Res "
        "    var x := b; return x; "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// ===========================================================================
// ACCEPT — a genuinely owned production is NOT over-rejected, and disposes
// the value EXACTLY once under the AOT+ASan/LSan oracle (Res.live == 0,
// surfaced through the process exit code — no leak, no double-free).
// ===========================================================================

// A fresh `new` production through an `owned` return — the ordinary factory
// shape this rule must not disturb.
BEZEL_TEST(owned_return_direct_borrow_3993,
           fresh_new_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Make(a: Res, b: Res): owned Res "
        "    return new Res(a.tag + b.tag); "
        "  end Make; "
        "  public static method Main(): int "
        "    do "
        "      using var a := new Res(3); using var b := new Res(4); "
        "      do using var c := Make(a, b); end; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// An `owned` parameter passed straight through — the legal transfer of a
// value the caller already handed over as owned.
BEZEL_TEST(owned_return_direct_borrow_3993,
           owned_param_passthrough_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Adopt(owned a: Res): owned Res return a; end Adopt; "
        "  public static method Main(): int "
        "    do using var c := Adopt(new Res(3)); end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// A relayed OWNED call result — the callee-returns-`owned`-T producer form.
BEZEL_TEST(owned_return_direct_borrow_3993,
           owned_call_result_relay_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Make(a: Res, b: Res): owned Res "
        "    return new Res(a.tag + b.tag); "
        "  end Make; "
        "  public static method Relay(a: Res, b: Res): owned Res return Make(a, b); end Relay; "
        "  public static method Main(): int "
        "    do "
        "      using var a := new Res(3); using var b := new Res(4); "
        "      do using var c := Relay(a, b); end; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// ===========================================================================
// NON-REGRESSION — the two ADJACENT borrow-return shapes each already have
// their OWN, more specific diagnostic; the #3993 fix must not double-report
// (or mis-report) either one.
// ===========================================================================

// `move` of a borrow through `owned` is checkOwnedReturnBorrowSource's
// (#1995) diagnostic, not this fix's — the fix explicitly excludes `move E`.
BEZEL_TEST(owned_return_direct_borrow_3993,
           move_of_borrow_keeps_1995_diagnostic_not_3993) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Take(b: Res): owned Res return move b; end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(jr, "cannot return a borrow as an `owned` value"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// A bare OWNED-field read through `owned` is checkOwnedFieldReturn's
// (§28.6 bullet 2) diagnostic, not this fix's.
BEZEL_TEST(owned_return_direct_borrow_3993,
           bare_owned_field_read_keeps_28_6_diagnostic_not_3993) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class Box "
        "  public owned f: Res? := null; "
        "  public constructor() this.f := new Res(1); end constructor; "
        "  public method TakeBare(): owned Res return f; end TakeBare; "
        "  destructor end destructor; "
        "end Box;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        jr, "a bare read of an owned field may not be returned"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}
