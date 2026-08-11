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

}  // namespace

// =====================================================================
// TRIPWIRE — NOT an assertion that the current behaviour is correct.
//
// §21.1 REQUIRES this program to be rejected: `Split` is declared
// `owned`-returning (stdlib §9.6) and §28.3's `null` row cannot demote the
// non-null path. It is accepted today ONLY because #2756 leaves the call
// untyped, so the predicate's result-head gate has nothing to read and fails
// open. The value is also never allocated, so there is no leak to measure yet.
//
// ⚠ WHEN THIS TEST GOES RED, #2756 HAS BEEN FIXED AND THE RIGHT RESPONSE IS TO
// FLIP IT: replace the FALSE below with TRUE, rename it to
// `nullcond_materializer_const_rejected_spec_21_1_stdlib_9_6`, and delete this
// banner. Do NOT weaken the predicate to keep this green — that would restore
// #2709's accept-and-leak on the `?.` path, this time with a real allocation
// behind it.
// =====================================================================
BEZEL_TEST(const_nullcond_materializer_2749,
           residual_nullcond_builtin_call_is_untyped_pending_2756) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method M(): int "
        "    var t: string? := \"a,b,c\"; "
        "    const b := t?.Split(\",\"); "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
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
