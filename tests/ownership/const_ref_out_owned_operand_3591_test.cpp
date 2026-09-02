// Ownership / diagnostics — §22.3's ref/out "no location of owned state" v1
// bound reaches a `ref`/`out` argument embedded in a runtime-`const`
// LOCAL DECLARATION's initializer, not only a `var` initializer / bare
// statement call. Found by review of #3591/#3419 (lane c4-d4), 2026-09-03.
//
// Spec, v0.286:
//   §22.3 "ref and out argument locations", verbatim: "A `ref` or `out`
//     argument must be an assignable storage location ... subject to: ...
//     two recorded v1 bounds (liftable): no location of owned state — an
//     owned local or `owned` parameter, an `owned` field, an owning-
//     collection or owning-inline-array slot — because a `ref` write is an
//     assignment through an alias, invisible to the drop-on-overwrite and
//     consume-site-freeze machinery ... key on syntactically visible stores."
//   §21.1 "Variable declarations", the runtime-`const` paragraph: the
//     initializer's TOP-LEVEL shape must not be an owned production; it says
//     nothing about ref/out writes EMBEDDED in sub-expressions of that
//     initializer — those are a §22.3 question, not a §21.1 one.
//
// THE BUG (measured pre-fix, matches the review repro exactly):
//
//   var  item := new Res(1); var   ok := F.Fill(out item);  → REJECTED (§22.3)
//   var  item := new Res(1); const ok := F.Fill(out item);  → ACCEPTED, LEAKS
//
// `checkRefOutOwnedOperand` (Ownership_Checks.cpp) is the §22.3 v1-bound
// enforcer; it is reached only through `recordMovesIn`'s CallExpr walk
// (recordMovesInArgs), which `MethodOwnership::handleLocalDecl` calls on a
// `var`'s initializer but which `visitStmt`'s `LocalConstDecl` arm never
// called on a `const`'s initializer at all — that arm only ever asked
// whether the WHOLE initializer's TOP-LEVEL shape is a rejected owned
// production (§21.1), never walked into the initializer's own
// sub-expressions. So an `out`/`ref` write to a pre-existing OWNED local,
// embedded in a const's initializer CALL, went completely unchecked: `item`
// gets silently overwritten (its displaced `Res(1)` untracked and leaked),
// and `scanReassignStmt`'s LocalConstDecl omission (a *separate*, correctly
// scoped exemption once this fix lands — Ownership.cpp's own
// `LOCALDECL-ONLY` marker on that site cites this enforcement) meant
// `reassignedLocals` never even learned `item` had been reassigned.
//
// THE FIX: `visitStmt`'s `LocalConstDecl` arm now calls
// `recordMovesIn(*lc->value)` — the same call `handleLocalDecl` makes for a
// `var` initializer — right before its `return;`, so `checkRefOutOwnedOperand`
// reaches a const's initializer arguments exactly as it already reaches a
// var's. Diagnostic-only for this shape (Fill returns `bool`, not an owned
// production, so §21.1's own top-level check does not fire either way) — no
// disposal/dataflow-state change, matching the project's LOUD > QUIET
// discipline: a silent leak becomes a compile-time reject, never the reverse.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// A class-typed `out` recipient (Res) and a static `Fill` that writes
// through it and returns `bool` — so §21.1's own top-level "owned
// production" check never fires for this initializer either way; the ONLY
// diagnostic in play is §22.3's ref/out v1 bound.
std::string prog(const std::string& binding) {
    return "module M; class Res public V: int := 0; end Res; "
           "class F "
           "  public static method Fill(out x: Res): bool "
           "    x := new Res(); return true; "
           "  end Fill; "
           "  public static method Main(): int "
           "    var item := new Res(); " +
           binding +
           "    return 0; "
           "  end Main; "
           "end F;";
}

// A value-typed (non-owned) `out` recipient, for the accept-controls: §22.3's
// v1 bound is scoped to OWNED state, so a plain `int` recipient must never be
// rejected by it, `const` or `var` alike.
std::string progValueTyped(const std::string& binding) {
    return "module M; class F "
           "  public static method Fill(out x: int): bool x := 5; return true; end Fill; "
           "  public static method Main(): int "
           "    var n: int := 0; " +
           binding +
           "    return 0; "
           "  end Main; "
           "end F;";
}

}  // namespace

// =============================================================================
// NEGATIVE — the const-initializer shape must be rejected with the exact
// §22.3 "owned local" text, matching the pre-existing var diagnostic.
// FAILS WITHOUT THE FIX: compiles clean (hasErrors() is false).
// =============================================================================
BEZEL_TEST(const_ref_out_owned_operand_3591,
           const_initializer_out_arg_to_owned_local_rejected_22_3) {
    auto r = parseAndCheckOwnership(prog("const ok := F.Fill(out item);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `out` argument may not be owned local `item`"));
}

// =============================================================================
// CONTROL — the var twin was already rejected before this fix and must stay
// rejected, with the identical diagnostic text (this fix must not have
// touched the var path at all).
// =============================================================================
BEZEL_TEST(const_ref_out_owned_operand_3591,
           var_initializer_out_arg_to_owned_local_rejected_22_3_control) {
    auto r = parseAndCheckOwnership(prog("var ok := F.Fill(out item);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `out` argument may not be owned local `item`"));
}

// =============================================================================
// ACCEPT CONTROLS — a non-owned (value-typed) `out` recipient is never
// rejected by §22.3's v1 bound, `const` or `var` alike; the fix must not
// widen `recordMovesIn`'s reach on a const initializer beyond the owned-state
// question it already enforces for `var`.
// =============================================================================
BEZEL_TEST(const_ref_out_owned_operand_3591,
           const_initializer_out_arg_to_value_local_accepted_control) {
    auto r = parseAndCheckOwnership(progValueTyped("const ok := F.Fill(out n);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(const_ref_out_owned_operand_3591,
           var_initializer_out_arg_to_value_local_accepted_control) {
    auto r = parseAndCheckOwnership(progValueTyped("var ok := F.Fill(out n);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// CONTROL — an ordinary const initializer with no ref/out argument anywhere
// is completely unaffected by wiring `recordMovesIn` into the LocalConstDecl
// arm (it must not spuriously reject or spuriously accept anything new).
// =============================================================================
BEZEL_TEST(const_ref_out_owned_operand_3591,
           ordinary_const_initializer_unaffected_control) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    const n := 5; "
        "    const m := n + 1; "
        "    return m; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
