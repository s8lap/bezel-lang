// Regression test for issue #8374 — `a ??= [1]` SILENTLY DROPPED the
// assignment.
//
// §7.7 (Null-coalescing `??` and `??=`) / §6.6 (the compound form) / §15.4
// (Collection literals) / §28.3 (the producers table) / §17.8 (the target's
// own type), v0.355.
//
// THE DEFECT (MEASURED on r30-c1 = 8f4dc616, the staging tip this fix
// branches from). `a ??= [1];` — a null-coalescing ASSIGNMENT whose RIGHT-HAND
// SIDE is a COLLECTION LITERAL — compiled, RAN, and returned the WRONG ANSWER:
//
//     s5.bzl:5:11: warning: internal: ??= short-circuit RHS expression failed
//       to lower (degraded — target retains its previous value) — #3025
//     ... and the program returns 0: `a` is still null after `a ??= [1];`.
//
// The assignment NEVER HAPPENED and nothing but a warning said so — a SILENT
// WRONG RESULT, the worst class of defect.
//
// THE CAUSE. `a CollectionLiteralExpr` is not a general expression value:
// `lowerExpr` returns null for one, because §15.4 gives a collection literal
// its target type FROM THE CONTEXT (the declaration / return type / parameter
// type). The `??=` lowering path had no context to give it — it lowered the
// right-hand side with a bare `lowerExpr` — so the literal did not lower and
// the #3025 backstop fired, preserving the target's previous value (null).
//
// This is the `??=` twin of #7879 (the `??` join's right operand), which is
// already fixed on this base: `lowerCoalesce` materializes a collection-literal
// right operand against the join's contextual target type.
//
// THE FIX. The `??=` lowering MATERIALIZES a collection-literal right-hand side
// against the TARGET's own type — §7.7's typing rule names it ("for a left
// operand of type `T?`, the right operand must be implicitly convertible (§5.1)
// to `T` or to `T?`"), and a collection literal is never nullable, so its
// target is `T` — through `lowerReturnCollectionLiteral`, the same shared
// "materialize this literal against this target type" helper the return /
// local-decl / call-argument / field-init / property-setter arms already use.
//
// §7.7 decides it: "The compound form `??=` evaluates its right-hand side, and
// invokes the setter, only when the assignment occurs" — so WHEN THE CONDITION
// HOLDS, THE ASSIGNMENT MUST HAPPEN.
//
// THE MATRIX. Rows 1, 3, 4, 9 and 12 are the shapes this fix restores (each
// was RED on the base). Rows 2, 5, 6, 7, 8 and 11 are the CONTROLS and pin
// that their verdicts are UNCHANGED — in particular that the `??` non-compound
// twin (row 11) stays fixed, and that a non-literal right-hand side, a string
// literal, a scalar literal and an array local are all untouched.
//
// NOTE (ownership, #8371 — CLOSED). §28.3.4's producers table classifies a
// collection literal targeting a reference type as OWNED and a plain
// (unmarked) parameter as BORROWED, so `a ??= [1]` is a MIXED meet that §7.7
// rejects wherever the meet is formed. The ownership pass did not classify it
// (a collection literal carries no `exprType` at any position), so it reported
// nothing and the fresh literal's backing was not discharged on a BORROWED
// target — the same gap #8371 filed for the `??` non-compound form, which
// leaked identically on THIS base (row 11 below leaked in both arms).
//
// #8371 is NOW CLOSED for the `??` (non-compound) meet: a `??` whose arm is a
// collection literal over a BORROW is refused with the §28.3 mixed-meet
// diagnostic, exactly as its `new` twin is. Row 11 below — the only row here
// that spells the `??` (non-compound) twin — therefore spells its meet with an
// OWNED left arm, and still measures what it measured before.
//
// The `??=` COMPOUND form (the rows above) is a DIFFERENT meet site and is
// NOT touched by the #8371 closure: `a ??= [1];` still assigns the literal
// into the borrowed target, and those rows still pass.
//
// These rows assert the IR-validity and the ASSIGNED VALUE, which is what
// #8374 is about; the ownership classification of the `??` non-compound meet
// was a separate defect in a separate pass, and that pass now rejects it.
//
//   T1 — the issue's own repro, VERBATIM: `a ??= [1];` over `Array<int>?`.
//        RED on the base: the program returned 0 (the assignment was dropped).
//        NEW: the program runs and returns 1 (`a.Count`).
//   T2 — the ALREADY-NON-NULL target: the right-hand side is NOT evaluated and
//        the target keeps its value (§7.7's short-circuit). CONTROL.
//   T3 — the LONGER literal `[1,2,3]`: the whole literal is stored.
//   T4 — the EMPTY literal `[]`: the empty array is stored (no crash, no drop).
//   T5 — CONTROL: a NON-LITERAL producer on the right-hand side
//        (`new Array<int>()`) is unchanged.
//   T6 — CONTROL: a STRING literal on the right-hand side is unchanged.
//   T7 — CONTROL: a SCALAR literal on the right-hand side is unchanged.
//   T8 — CONTROL: an ARRAY LOCAL on the right-hand side is unchanged.
//   T9 — the FIELD target (`h.A ??= [1];`): the target is a field, not a
//        local.
//   T10 — the ELEMENT target (`h.A[0] ??= [1];`): the accessor-mediated
//         `??=` path, which is a DIFFERENT lowering site (and a different
//         file). Its verdict is unchanged by this fix and is pinned here.
//   T11 — CONTROL: the `??` (NON-COMPOUND) twin `var b := a ?? [1];` — #7879
//         fixed this on this very base; it must STAY fixed. The LEFT arm is
//         an OWNED producer (`owned Array<int>?`), because the #8371 closure
//         makes a collection-literal arm over a BORROW a rejected mixed meet.
//   T12 — the NULLABLE-ELEMENT target (`Array<int?>?`): the literal's element
//         type follows the target's.
//   T13 — the `internal:` WARNING must be GONE for the fixed shape: the
//         issue's expected behaviour is "NO `internal:` warning".

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::emitAndVerify;
using bezel::test::runAot;

namespace {

// The issue's own repro, VERBATIM.
constexpr const char* kRepro =
    "module M; import System;\n"
    "public class C\n"
    "  public static method Bad(a: Array<int>?): int\n"
    "    a ??= [1];\n"
    "    if a = null then return 0; end if;\n"
    "    return a.Count;\n"
    "  end Bad;\n"
    "  public static method Main(): int\n"
    "    return C.Bad(null);\n"
    "  end Main;\n"
    "end C;\n";

// The count of `internal:` diagnostics in a diagnostic engine — the shape of
// the #3025 backstop note this fix must NOT emit for a `??=` whose right-hand
// side is a collection literal.
std::size_t internalDiagnosticCount(const bezel::DiagnosticEngine& diags) {
    std::size_t n = 0;
    for (const auto& d : diags.diagnostics()) {
        if (d.message.find("internal:") != std::string::npos) ++n;
    }
    return n;
}

}  // namespace

// ===========================================================================
// T1 — the issue's own repro, verbatim. RED on the base: the program RAN and
// returned 0 (`a` was still null — the assignment never happened), with the
// #3025 "??= short-circuit RHS expression failed to lower" warning.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           collection_literal_rhs_is_assigned_sec_7_7) {
    auto r = runAot(kRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(internalDiagnosticCount(r.diagnostics), 0u);
}

// ===========================================================================
// T2 — CONTROL: the ALREADY-NON-NULL target. §7.7: "the right operand is
// evaluated only when the left operand's value is `null`" — so the right-hand
// side is NOT evaluated and the target keeps its value (three elements). The
// base's #3025 warning fired for this shape too (a lowering-time diagnostic
// about a right-hand side that was never needed); the fix removes it.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           non_null_target_keeps_its_value_sec_7_7) {
    auto r = runAot(
        "module M8374b; import System;\n"
        "public class C\n"
        "  public static method Bad(a: Array<int>?): int\n"
        "    a ??= [1];\n"
        "    if a = null then return -1; end if;\n"
        "    return a.Count;\n"
        "  end Bad;\n"
        "  public static method Main(): int\n"
        "    return C.Bad([7,8,9]);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ===========================================================================
// T3 — the LONGER literal: the WHOLE literal is stored, and the target's
// element type is honoured (three `int` elements).
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           longer_literal_is_stored_whole_sec_15_4) {
    auto r = runAot(
        "module M8374c; import System;\n"
        "public class C\n"
        "  public static method Bad(a: Array<int>?): int\n"
        "    a ??= [1,2,3];\n"
        "    if a = null then return -1; end if;\n"
        "    return a.Count;\n"
        "  end Bad;\n"
        "  public static method Main(): int\n"
        "    return C.Bad(null);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ===========================================================================
// T4 — the EMPTY literal: the empty array is stored — not a crash, not a
// drop. The count is 0, and the target is non-null afterwards (the program
// returns 0 rather than the -1 it would return for a dropped assignment).
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           empty_literal_is_stored_sec_15_4) {
    auto r = runAot(
        "module M8374d; import System;\n"
        "public class C\n"
        "  public static method Bad(a: Array<int>?): int\n"
        "    a ??= [];\n"
        "    if a = null then return -1; end if;\n"
        "    return a.Count;\n"
        "  end Bad;\n"
        "  public static method Main(): int\n"
        "    return C.Bad(null);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// T5 — CONTROL: a NON-LITERAL producer on the right-hand side
// (`new Array<int>()`) already lowered and must NOT be regressed.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           non_literal_producer_is_unchanged_sec_7_7) {
    auto r = runAot(
        "module M8374e; import System;\n"
        "public class C\n"
        "  public static method Bad(a: Array<int>?): int\n"
        "    a ??= new Array<int>();\n"
        "    if a = null then return -1; end if;\n"
        "    return a.Count;\n"
        "  end Bad;\n"
        "  public static method Main(): int\n"
        "    return C.Bad(null);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// T6 — CONTROL: a STRING literal on the right-hand side. A string literal is
// NOT a collection literal; the target's own type governs what `??=` may
// store (§17.8 / §28.3), so the target here is `string?`.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           string_literal_rhs_is_unchanged_sec_17_8) {
    auto r = runAot(
        "module M8374f; import System;\n"
        "public class C\n"
        "  public static method Bad(a: string?): int\n"
        "    a ??= \"x\";\n"
        "    if a = null then return -1; end if;\n"
        "    return a.Length;\n"
        "  end Bad;\n"
        "  public static method Main(): int\n"
        "    return C.Bad(null);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// T7 — CONTROL: a SCALAR literal on the right-hand side, over an `int?`
// target.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           scalar_literal_rhs_is_unchanged_sec_17_8) {
    auto r = runAot(
        "module M8374g; import System;\n"
        "public class C\n"
        "  public static method Bad(a: int?): int\n"
        "    a ??= 7;\n"
        "    if a = null then return -1; end if;\n"
        "    return a;\n"
        "  end Bad;\n"
        "  public static method Main(): int\n"
        "    return C.Bad(null);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// T8 — CONTROL: an ARRAY LOCAL on the right-hand side.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           array_local_rhs_is_unchanged_sec_7_7) {
    auto r = runAot(
        "module M8374h; import System;\n"
        "public class C\n"
        "  public static method Bad(a: Array<int>?): int\n"
        "    var otherArray: Array<int> := [4,5,6];\n"
        "    a ??= otherArray;\n"
        "    if a = null then return -1; end if;\n"
        "    return a.Count;\n"
        "  end Bad;\n"
        "  public static method Main(): int\n"
        "    return C.Bad(null);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ===========================================================================
// T9 — the FIELD target: `h.A ??= [1];` — the target is a field, not a
// local. The same `??=` lowering arm owns it, so the assignment happens.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           field_target_is_assigned_sec_7_7) {
    auto r = runAot(
        "module M8374i; import System;\n"
        "public class H\n"
        "  public A: Array<int>? := null;\n"
        "end H;\n"
        "public class C\n"
        "  public static method Bad(): int\n"
        "    using var h := new H();\n"
        "    h.A ??= [1];\n"
        "    if h.A = null then return -1; end if;\n"
        "    return h.A!.Count;\n"
        "  end Bad;\n"
        "  public static method Main(): int\n"
        "    return C.Bad();\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// T10 — the ELEMENT target: `h.A[0] ??= [1];` — an ACCESSOR-MEDIATED
// `??=` target. That shape is claimed by a DIFFERENT lowering site (the
// accessor-mediated `??=` path, which reports #2905), and this fix does not
// reach it. Its verdict is pinned here so a future change to that site is
// visible from this file.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           element_target_verdict_is_unchanged_sec_7_7) {
    auto r = emitAndVerify(
        "module M8374j; import System;\n"
        "public class H\n"
        "  public A: Array<Array<int>?> := [null];\n"
        "end H;\n"
        "public class C\n"
        "  public static method Bad(): int\n"
        "    using var h := new H();\n"
        "    h.A[0] ??= [1];\n"
        "    return 0;\n"
        "  end Bad;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.lowered);
}

// ===========================================================================
// T11 — CONTROL: the `??` (NON-COMPOUND) twin `var b := a ?? [1];`. #7879
// fixed this on this very base (`a638afa8`, on `r30-c1`) — it must STAY
// fixed.
//
// The LEFT arm is an OWNED producer (`owned Array<int>?`) rather than a
// borrowed parameter. Section 28.3's `a ?? b` row makes the meet of the two
// arms' ownership states, and its collection-literal row makes the literal
// arm OWNED: a literal arm over a BORROW left is a MIXED meet, and section
// 28.3 K7.3 rejects a mixed meet wherever it is formed. That is the #8371
// closure — the gap this file's header NOTE recorded as "the same gap #8371
// filed for the `??` non-compound form" — so the borrowed-left spelling of
// this meet no longer compiles. The OWNED left keeps the row's claim intact
// (the `??` twin still lowers to a terminated join and still yields the
// literal) on a shape that compiles on both arms.
//
// NOTE: the `??=` COMPOUND form is a different meet site and is NOT touched
// by the #8371 closure — its rows above (T1–T5, T9, T12) still spell the
// borrowed target `a ??= [1];` and still pass.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           coalesce_twin_stays_fixed_sec_7_7) {
    auto r = runAot(
        "module M8374k; import System;\n"
        "public class C\n"
        "  public static method Fresh(): owned Array<int>?\n"
        "    return null;\n"
        "  end Fresh;\n"
        "  public static method Bad(): int\n"
        "    var b := Fresh() ?? [1];\n"
        "    if b = null then return -1; end if;\n"
        "    return b.Count;\n"
        "  end Bad;\n"
        "  public static method Main(): int\n"
        "    return C.Bad();\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// T12 — the NULLABLE-ELEMENT target (`Array<int?>?`): the literal's element
// type follows the target's.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           nullable_element_target_is_assigned_sec_15_4) {
    auto r = runAot(
        "module M8374l; import System;\n"
        "public class C\n"
        "  public static method Bad(a: Array<int?>?): int\n"
        "    a ??= [1];\n"
        "    if a = null then return -1; end if;\n"
        "    return a.Count;\n"
        "  end Bad;\n"
        "  public static method Main(): int\n"
        "    return C.Bad(null);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// T13 — the `internal:` WARNING must be GONE. The issue's expected behaviour
// is "NO `internal:` warning" — the #3025 backstop note is for shapes that
// CANNOT lower, and this shape now lowers.
// ===========================================================================
BEZEL_TEST(codegen_coalesce_assign_collection_literal_8374,
           no_internal_warning_for_the_fixed_shape_sec_7_7) {
    auto r = emitAndVerify(
        "module M8374m;\n"
        "public class C\n"
        "  public static method Bad(a: Array<int>?): int\n"
        "    a ??= [1];\n"
        "    if a = null then return 0; end if;\n"
        "    return a.Count;\n"
        "  end Bad;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.lowered);
    BEZEL_EXPECT_TRUE(r.verifies);
    BEZEL_EXPECT_EQ(internalDiagnosticCount(r.diagnostics), 0u);
}
