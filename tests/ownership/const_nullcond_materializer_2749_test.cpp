// Ownership / diagnostics — §21.1's `const` rejection and the NULL-CONDITIONAL
// call spelling `recv?.M(..)`. Issue #2749 item 3.
//
// ⚠ READ THIS BEFORE READING THE ASSERTIONS. This file does NOT pin a fixed
// defect. It pins a HALF-fix plus the measured reason the other half is
// impossible here, and one of its cases is a TRIPWIRE that is *meant* to go red
// when the blocker (#2756) is fixed. Both are labelled in place.
//
// Spec, v0.240:
//   §21.1 "Variable declarations", Ownership paragraph, verbatim: "Rejected,
//     with a diagnostic naming this rule and the `using var` remedy: a fresh
//     `new`, an `owned`-returning call, […]."
//   stdlib §9.6 declares `string.Split` `owned`-returning; §4.4 does the same
//     for `Sorted`/`Reversed`/`Slice`.
//   §28.3's `null` row, verbatim: "`null` has no value to own, so it is
//     **compatible with either an owned target or a borrowed target**" — so the
//     null path a `?.` adds cannot demote the non-null path's owned production.
//     `s?.Split(",")` is an `owned`-returning call and §21.1 rejects it.
//
// WHAT #2749 ITEM 3 REPORTED. `ast::NullCondMemberExpr` (`src/ast/Node.hpp`) is a
// SIBLING of `ast::MemberAccessExpr`, not a subclass, so
// `common::isBuiltinOwnedMaterializerCall`'s `dynamic_cast<MemberAccessExpr*>`
// callee gate returned false for every `?.` spelling. The suggested remedy was
// sema's own #2581 precedent, `ExprTC::builtinInstanceCallee` ("a builtin
// instance-call callee is EITHER a MemberAccessExpr OR a NullCondMemberExpr").
//
// WHAT WAS ACTUALLY THERE. The callee gate was only the FIRST of two blockers,
// and the second is the operative one. MEASURED on 6cd9b70 with temporary
// instrumentation inside the predicate — with the callee gate widened, the
// predicate IS reached for `a?.Sorted()`, `a?.Reversed()`, `a?.Slice(0..1)` and
// `t?.Split(",")`, each with NO symbol on the callee (so narrowness passes them),
// but each with `ctx.exprType(call) == nullptr`:
//
//   member=Sorted   isNullCond=1 sym=(nil)  exprType=(nil)   ← builtin, UNTYPED
//   member=Reversed isNullCond=1 sym=(nil)  exprType=(nil)   ← builtin, UNTYPED
//   member=Slice    isNullCond=1 sym=(nil)  exprType=(nil)   ← builtin, UNTYPED
//   member=Split    isNullCond=1 sym=(nil)  exprType=(nil)   ← builtin, UNTYPED
//   member=Sorted   isNullCond=1 sym=0x…    exprType=0x…     ← USER, typed fine
//
// SEMA NEVER TYPES A BUILTIN INSTANCE CALL REACHED THROUGH `?.`. The same four
// lines each drew the #2434 "failed to lower to a value (degraded to a
// zero-initialized value)" internal diagnostic; the user-method line drew none.
// Filed as #2756. The predicate's result-head gate reads that null type and, by
// the deliberate fail-open polarity it inherits from #2709, ACCEPTS.
//
// SO THERE IS NO FAIL->PASS DEMONSTRATION FOR ITEM 3, and this file does not
// pretend otherwise. The widening is shipped for a soundness reason: today
// `const b := s?.Split(",");` is not a leak (nothing is allocated — the local
// degrades to zero), but the moment #2756 lands the lowering it becomes a REAL
// accept-and-leak of exactly #2709's shape, and a lane scoped to "make `?.`
// builtin calls lower" would have no reason to add the §21.1 rejection too.
// Pre-placing the guard means that fix arrives already covered.
//
// The one thing that IS mechanically demonstrable today is the NARROWNESS
// premise the widening rests on, and it needed re-establishing rather than
// assuming, because `?.` is typed by a separate function with its own resolution
// mirror: `ExprTC::resolveNullCondMember` calls `setExprSymbol(&nm, member)`
// unconditionally once a member resolved, and returns early attaching nothing
// when none did. That is what keeps user methods and function-typed fields out
// of the predicate on the `?.` path — the same single line that keeps them out
// on the `.` path.

#include <string>

#include "support/aot_support.hpp"       // runAot / runAotChecked
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// ANY §21.1 const rejection, whichever branch produced it.
bool hasAnyConstOwnedProductionError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("must not take an owned production") !=
                std::string::npos ||
            d.message.find("must not take a deferred-activation producer") !=
                std::string::npos)
            return true;
    }
    return false;
}

// narrow #3376: the "codegen has no lowering for this `?.` member" rejection.
bool hasNotYetSupportedInCodegen(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("is not yet supported in codegen") !=
            std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// =====================================================================
// fix #2756 / §21.1 / stdlib §9.6 (v0.282): `const b := t?.Split(",")` is now
// rejected. Before this fix the call left the `NullCondMemberExpr` access
// untyped (resolveNullCondMember had no `builtinValueMemberType` escape hatch
// for built-in methods and no colName dispatch of its own) and the call's
// exprType at typeCall was null too, so the §21.1 predicate's result-head
// gate had nothing to read and the program silently degraded to a
// zero-initialized local (#2434). This test pins the rejection: the
// materializer predicate (isBuiltinOwnedMaterializerCall,
// OwnershipFacts.cpp:450) now sees the typed call (Split, no symbol, head
// `Array`), §21.1 fires ("must not take an owned production"), and the
// `const` initializer is rejected.
//
// The matched tripwire was the file's `residual_nullcond_builtin_call_is_untyped_pending_2756`
// test, which asserted the pre-fix untyped-and-accepted state and whose banner
// asked for THIS exact replacement on #2756's fix. The user-method control
// case (`accept_user_method_via_nullcond_spec_28_3_21_1`) stays green because
// `resolveNullCondMember` attaches a MethodSymbol to the callee and the same
// narrowness gate that excludes the `.` form excludes the `?.` form.
// =====================================================================
BEZEL_TEST(const_nullcond_materializer_2749,
           nullcond_materializer_const_rejected_spec_21_1_stdlib_9_6) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method M(): int "
        "    var t: string? := \"a,b,c\"; "
        "    const b := t?.Split(\",\"); "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasAnyConstOwnedProductionError(r));
}

// =====================================================================
// NARROWNESS, on the `?.` path — the half that IS demonstrable.
//
// A USER method named one of the eight materializers, reached through `?.`,
// returning a plain (non-`owned`) borrow of a field. §28.3 classifies it as
// Borrowed and §21.1 admits borrows verbatim ("Admitted: […] and **borrows**"),
// so it must keep compiling as a `const` initializer. It is excluded because
// `resolveNullCondMember` attaches a MethodSymbol to the callee — measured:
// `sym=0x…, exprType=0x…` on the trace above, versus `sym=(nil)` for the four
// builtins.
//
// This case goes RED on the over-wide predicate the #2709 header warns about
// (`dynamic_cast<sema::MethodSymbol*>` instead of `exprSymbol == nullptr`)
// combined with a result-head-only gate — i.e. on the natural wrong widening of
// item 3.
// =====================================================================
BEZEL_TEST(const_nullcond_materializer_2749,
           accept_user_method_via_nullcond_spec_28_3_21_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public _sorted: Array<int>; "
        "  public method Sorted(): Array<int> => _sorted; "
        "  public constructor(a: Array<int>) _sorted := a; end constructor; "
        "end Box; "
        "class C public static method M(): int "
        "    using var src := new Array<int>(); "
        "    src.Add(3); src.Add(1); "
        "    using var bx := new Box(src); "
        "    var bxn: Box? := bx; "
        "    const s := bxn?.Sorted(); "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// The `.` spelling of the very same user method — the control proving the case
// above is testing the `?.` path specifically and not merely a user method's
// blanket admissibility.
BEZEL_TEST(const_nullcond_materializer_2749,
           accept_user_method_via_dot_spec_28_3_21_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public _sorted: Array<int>; "
        "  public method Sorted(): Array<int> => _sorted; "
        "  public constructor(a: Array<int>) _sorted := a; end constructor; "
        "end Box; "
        "class C public static method M(): int "
        "    using var src := new Array<int>(); "
        "    src.Add(3); src.Add(1); "
        "    using var bx := new Box(src); "
        "    const s := bx.Sorted(); "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// The `.` spelling of a BUILTIN materializer still rejects — #2709's fix is
// untouched by the callee-shape widening. If this ever goes green-to-red, the
// widening broke the shape it was extending rather than extending it.
BEZEL_TEST(const_nullcond_materializer_2749,
           dot_spelling_builtin_still_rejected_2709_unchanged) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method M(): int "
        "    var t: string := \"a,b,c\"; "
        "    const b := t.Split(\",\"); "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasAnyConstOwnedProductionError(r));
}

// =====================================================================
// fix #2756 / §7.5 (v0.282): the `?.` builtin-call typing fix above is the
// PRECONDITION for the `?.` builtin-call surface to type at all. Before
// this fix `var parts := t?.Split(",");` (with `t: string?` and a non-const
// initializer) was left untyped on the access AND on the call — codegen
// degraded the local to a zero-initialized slot (#2434). After the fix the
// access is typed `Array<string>?` (per §7.5) and the call's exprType is
// propagated by the NCM-block shim in typeCall. This test pins the post-
// fix sema/ownership behaviour on the NON-`const` shape (the `const` shape
// is pinned by `nullcond_materializer_const_rejected_spec_21_1_stdlib_9_6`
// above): ownership parses cleanly, the §21.1 predicate's narrowness gate
// does not fire (this is a `var`, not a `const`), and the diagnostic stream
// does NOT carry the §21.1 \"must not take an owned production\" message.
// The codegen-level \"degraded to a zero/null return value\" #2434/#2491
// diagnostic is what codegen emits when the call exprType is null; the
// OWNERSHIP pass itself does not produce it, so we rely on the matching
// non-`const` call as a positive witness that the type is now non-null.
// =====================================================================
BEZEL_TEST(const_nullcond_materializer_2749,
           nullcond_split_owned_var_no_const_reject_spec_7_5) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method M(): int "
        "    var t: string? := \"a,b,c\"; "
        "    var parts := t?.Split(\",\"); "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    // Non-const binding of an owned production: §21.1 does NOT reject
    // (it only rejects `const`). §28.10's auto-drop note MAY fire here
    // (the value is owned and not `using`-bound), and that is expected;
    // we only assert §21.1 is silent.
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// =====================================================================
// narrow #3376 / §7.5 (v0.282) — THE REGRESSION THIS FILE PREVIOUSLY MISSED.
//
// #3376 pre-typed `t?.Split(",")` in `resolveNullCondMember` so the §21.1
// materializer predicate could read a non-null call exprType (the `const`
// rejection pinned above). But it ALSO returned early from that block, which
// skipped the string-receiver gate further down — the gate that reports
// "string member `?.Split` is not yet supported in codegen".
//
// Codegen has no lowering for `?.Split`; only the diagnostic saying so went
// away. Measured on `var parts := t?.Split(",")`:
//
//   main (no #3376) -> "string member `?.Split` is not yet supported in
//                       codegen", rc=1, program REJECTED
//   with #3376      -> "#2434 ... degraded to a zero-initialized value",
//                       rc=0, program ACCEPTED carrying a wrong value
//
// A loud compile error had become a silent wrong value — strictly worse than
// the #2434 degradation #3376 set out to fix. Neither shipped #3376 test could
// see it: both assert only on the §21.1 message, one that it is present and
// one that it is absent, and the codegen-unsupported error is a different
// message entirely.
//
// `narrow #3376` keeps the type (so the §21.1 `const` rejection above still
// fires) and lets the gate reject as well. These two tests pin BOTH halves of
// that invariant so it cannot silently invert again.
//
// BEHAVIOR FLIP (fix #5448, §9.6 / §7.5, v0.315): the `?.Split` call is now
// LOWERED by codegen (no longer hits the "not yet supported" gate). The var
// case now COMPILES correctly, and the const case only gets the ownership
// error (Split returns owned Array<string>), not the codegen error.
// The test names and assertions below reflect the NEW correct behavior.
// =====================================================================
// fix #5448 (§9.6 / §7.5, v0.315): `var parts := t?.Split(",")` now compiles
// and runs correctly. The codegen gate no longer fires; §21.1 is silent on
// non-const bindings.
BEZEL_TEST(const_nullcond_materializer_2749,
           nullcond_split_var_now_accepted_spec_7_5) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method M(): int "
        "    var t: string? := \"a,b,c\"; "
        "    var parts := t?.Split(\",\"); "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    // With the fix, codegen lowers Split and the program compiles cleanly.
    BEZEL_EXPECT_FALSE(hasNotYetSupportedInCodegen(r));
    // §21.1 is still correctly silent on a non-`const` binding.
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// fix #5448 (§9.6 / §7.5, v0.315): `const parts := t?.Split(",")` is rejected
// by §21.1 (Split returns owned Array<string>, const cannot hold owned) but
// NOT by the codegen gate (which no longer fires for Split).
BEZEL_TEST(const_nullcond_materializer_2749,
           nullcond_split_const_keeps_ownership_error_only_spec_21_1_7_5) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method M(): int "
        "    var t: string? := \"a,b,c\"; "
        "    const parts := t?.Split(\",\"); "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    // §21.1 const rejection is PRESERVED (the exprType is still set by sema).
    BEZEL_EXPECT_TRUE(hasAnyConstOwnedProductionError(r));
    // The codegen gate no longer fires — Split is now lowered correctly.
    BEZEL_EXPECT_FALSE(hasNotYetSupportedInCodegen(r));
}
