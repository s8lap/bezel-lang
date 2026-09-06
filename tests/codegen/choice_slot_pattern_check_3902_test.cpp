// Regression tests for #3902 (§35.5 "Case patterns", v0.294): a choice-case
// positional destructuring pattern's REFUTABLE payload slots (constants,
// ranges, relationals, alternative lists, nested destructuring — anything
// other than a naked/`var` binding or `_`) were never checked at codegen.
//
// §35.5's "Case patterns" bullet is explicit: "All §19.3 slot rules carry
// over: naked identifiers bind, `var x` binds explicitly, `_` discards per
// slot, per-slot `|` alternatives among binding-free members, NESTED
// PATTERNS RECURSE." `Emitter::patternMatches`'s choice-scrutinee
// DestructuringPattern branch (LlvmBackend_Exprs2.cpp) recursed into
// `patternMatches` for a RECORD scrutinee's non-binding sub-patterns
// (mirrors §19.3), but for a CHOICE scrutinee only extracted and bound
// naked/`var` payload slots — every other slot kind was silently skipped
// and the match reduced to pure tag equality, so ANY refutable payload
// slot pattern matched unconditionally once the case tag matched. This is
// a SILENT WRONG ANSWER: the program compiles, runs, and takes the wrong
// `case` arm with no diagnostic.
//
// Root repro (measured before the fix, both returning the WRONG value 7
// instead of the correct 3):
//   var w := Wrap.Val(5, 99);
//   return (case w of Val(5, 1) => 7, _ => 3);      // constant slot
//   var w := Wrap.Val(99, 1);
//   return (case w of Val(1..5, 1) => 7, _ => 3);   // range slot
//
// Every row below is a MEASURED pair: a "checked" row (the refutable slot
// does not match, proving the check now runs) and a "control" row (the
// same pattern legitimately matches, proving the fix did not turn the slot
// into an always-reject). A row that only asserts one direction is not
// evidence the guard is sensitive to the payload value (§ Lane rule "a row
// that passes is not evidence until something has shown it can fail").

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Wrap a method body in the standard module+decls+class shell and run it;
// return the exit code (-999 if the run failed). Mirrors
// choice_codegen_35_test.cpp's `runDecls`.
int runDecls(const std::string& decls, const std::string& body) {
    auto r = runAot(
        std::string("module M; ") + decls +
        std::string(" class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

const char kWrapDecl[] = "choice Wrap Val(X: int, Y: int), None; end Wrap;";

}  // namespace

// --- §35.5 / §19.3: a CONSTANT payload slot must be checked -----------------

// fix #3902 primary repro: `w.X = 5` matches the `5` slot but `w.Y = 99`
// does NOT match the `1` slot — the arm must NOT be selected. Before the
// fix this returned 7 (the arm matched unconditionally once the tag
// matched); the correct value is the `_` fallthrough, 3.
BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_constant_slot_mismatch_falls_through) {
    auto r = runDecls(kWrapDecl,
        "var w := Wrap.Val(5, 99); "
        "return (case w of Val(5, 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 3);
}

// Control: the same pattern over a payload that DOES satisfy every
// constant slot must still match (7) — the fix must not turn the slot
// into an unconditional reject.
BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_constant_slot_match_selects_arm) {
    auto r = runDecls(kWrapDecl,
        "var w := Wrap.Val(5, 1); "
        "return (case w of Val(5, 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 7);
}

// --- §35.5 / §19.3 / §16.3: a RANGE payload slot must be checked ------------

// fix #3902 second repro: `w.X = 99` is not in `1..5`; the arm must not
// select. Before the fix this returned 7.
BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_range_slot_out_of_bounds_falls_through) {
    auto r = runDecls(kWrapDecl,
        "var w := Wrap.Val(99, 1); "
        "return (case w of Val(1..5, 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 3);
}

// Control: a value within the range slot's bounds still matches.
BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_range_slot_in_bounds_selects_arm) {
    auto r = runDecls(kWrapDecl,
        "var w := Wrap.Val(3, 1); "
        "return (case w of Val(1..5, 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 7);
}

// --- §35.5 / §19.3: a RELATIONAL payload slot must be checked ---------------

BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_relational_slot_not_satisfied_falls_through) {
    auto r = runDecls(kWrapDecl,
        "var w := Wrap.Val(5, 1); "
        "return (case w of Val(> 10, 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 3);
}

BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_relational_slot_satisfied_selects_arm) {
    auto r = runDecls(kWrapDecl,
        "var w := Wrap.Val(20, 1); "
        "return (case w of Val(> 10, 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 7);
}

// --- §35.5 / §4081 (alternative list): a `|` payload slot must be checked --

BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_alternative_list_slot_none_match_falls_through) {
    auto r = runDecls(kWrapDecl,
        "var w := Wrap.Val(7, 1); "
        "return (case w of Val(5 | 6, 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 3);
}

BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_alternative_list_slot_member_matches_selects_arm) {
    auto r = runDecls(kWrapDecl,
        "var w := Wrap.Val(6, 1); "
        "return (case w of Val(5 | 6, 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 7);
}

// --- Per-slot AND: a refutable slot alongside a binding slot ----------------

// Slot 0 is a `var x` (always binds); slot 1 is a constant `1` that must
// still be checked independently. Before the fix, the ENTIRE destructuring
// pattern reduced to tag equality regardless of how many slots were
// binding vs. refutable — this pins that a non-binding slot is checked
// even when a sibling slot is a naked/`var` binding.
BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_binding_slot_does_not_mask_sibling_constant_check) {
    auto r = runDecls(kWrapDecl,
        "var w := Wrap.Val(5, 99); "
        "return (case w of Val(var x, 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 3);
}

// Same shape, but the sibling constant slot DOES match — and the binding
// slot must still have bound its value correctly (x == 5, so x + 100 ==
// 105), proving the fix's AND-fold does not disturb the existing binding
// path.
BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_binding_slot_still_binds_when_sibling_matches) {
    auto r = runDecls(kWrapDecl,
        "var w := Wrap.Val(5, 1); "
        "return (case w of Val(var x, 1) => x + 100, _ => 3);");
    BEZEL_EXPECT_EQ(r, 105);
}

// --- §35.5 / §19.3: an ENUM-CONSTANT payload slot must be checked -----------
//
// A bare identifier in destructuring-slot position is a NAKED BINDING
// (A.7's "Identifier" alternative), not a constant reference — so an enum
// case name must be spelled QUALIFIED (`Color.Green`) in a payload slot to
// be unambiguous with a fresh binding. This is the same rule the record
// destructuring path already observes; it is not part of #3902's fix.

BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_enum_slot_mismatch_falls_through) {
    auto r = runDecls(
        "enum Color Red, Green, Blue; end Color; "
        "choice EWrap Val(C: Color), None; end EWrap;",
        "var w := EWrap.Val(Color.Red); "
        "return (case w of Val(Color.Green) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 3);
}

BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_enum_slot_match_selects_arm) {
    auto r = runDecls(
        "enum Color Red, Green, Blue; end Color; "
        "choice EWrap Val(C: Color), None; end EWrap;",
        "var w := EWrap.Val(Color.Green); "
        "return (case w of Val(Color.Green) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 7);
}

// --- §35.5 / §19.3: a STRING CONSTANT payload slot must be checked ----------
// (content equality, not the choice-slot's own `bz_string*` pointer.)

BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_string_slot_mismatch_falls_through) {
    auto r = runDecls(
        "choice SWrap Val(S: string, Y: int), None; end SWrap;",
        "var s := \"xyz\"; "
        "var w := SWrap.Val(s, 1); "
        "return (case w of Val(\"abc\", 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 3);
}

BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_string_slot_content_match_selects_arm) {
    auto r = runDecls(
        "choice SWrap Val(S: string, Y: int), None; end SWrap;",
        "var s := \"abc\"; "
        "var w := SWrap.Val(s, 1); "
        "return (case w of Val(\"abc\", 1) => 7, _ => 3);");
    BEZEL_EXPECT_EQ(r, 7);
}

// --- §35.5 "nested patterns recurse": a NESTED DestructuringPattern slot ----
//
// A choice payload slot that is itself a record destructuring pattern
// (`Wrap(Inner(5))`) is a non-binding sub-pattern of the outer
// DestructuringPattern, so it fell into the same silently-skipped bucket
// as a plain constant slot before the fix. These rows use a discard/
// constant inner slot (not a binding) — nested BINDING inside a choice
// payload slot (`Wrap(Inner(v)) => v`) hits a separate, pre-existing sema
// gap (§19.3's per-slot LocalSymbol registration has no nested-destructure
// walk on the choice side, unlike the record side's) and is filed
// separately; it is not part of this fix.

BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_nested_destructure_slot_mismatch_falls_through) {
    auto r = runDecls(
        "record Inner(V: int); "
        "choice Outer NWrap(inner: Inner), None; end Outer;",
        "var o := Outer.NWrap(new Inner(9)); "
        "return (case o of NWrap(Inner(5)) => 7, None => 0, _ => 3);");
    BEZEL_EXPECT_EQ(r, 3);
}

BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_nested_destructure_slot_match_selects_arm) {
    auto r = runDecls(
        "record Inner(V: int); "
        "choice Outer NWrap(inner: Inner), None; end Outer;",
        "var o := Outer.NWrap(new Inner(5)); "
        "return (case o of NWrap(Inner(5)) => 7, None => 0, _ => 3);");
    BEZEL_EXPECT_EQ(r, 7);
}

// A nested `_` discard still matches unconditionally (no regression on the
// nested-recursion path for the irrefutable case).
BEZEL_TEST(codegen_choice_slot_check_3902,
           spec_35_5_nested_destructure_discard_always_selects_arm) {
    auto r = runDecls(
        "record Inner(V: int); "
        "choice Outer NWrap(inner: Inner), None; end Outer;",
        "var o := Outer.NWrap(new Inner(42)); "
        "return (case o of NWrap(Inner(_)) => 7, None => 0, _ => 3);");
    BEZEL_EXPECT_EQ(r, 7);
}
