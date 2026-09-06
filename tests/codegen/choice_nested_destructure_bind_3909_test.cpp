// Regression tests for #3909 (§35.5 "Case patterns" / §19.3, v0.294): a
// NESTED `DestructuringPattern` sub-pattern inside a choice-case payload slot
// (`Wrap(Inner(v))`) could not BIND a name — sema rejected the arm body's
// reference to `v` with "unresolved identifier `v`", even though the
// identical shape over a RECORD scrutinee (`Outer(Inner(v)) => v`) already
// worked, and even though a NESTED CHECK in the same choice-payload position
// (`Wrap(Inner(5))`, `Wrap(Inner(_))`) already worked — fix #3902 (this
// cycle) made the codegen-side match CHECK recurse correctly; this issue is
// the analogous sema-side gap: nothing ever registered a `LocalSymbol` for a
// name bound inside a nested destructuring sub-pattern one level below a
// choice case's own payload slots.
//
// Root cause (`src/sema/ExprTypecheck.cpp` CaseStmt handler and its CaseExpr
// twin in `src/sema/ExprTypecheck_Exprs.cpp`): the `scrutIsChoice` per-slot
// loop registered a `LocalSymbol` only for a TOP-LEVEL slot's own naked/`var`
// binding. The RECORD-scrutinee `DestructuringPattern` handler a few lines
// below has, and has always had, a second walk (`nestedWork`, historically
// #726) that recurses into a nested `DestructuringPattern` sub-pattern and
// registers a `LocalSymbol` for each of ITS naked/`var` bindings — the choice
// branch never had an equivalent.
//
// THE FIX IS A DEDUP, NOT A THIRD COPY: before this fix, that `nestedWork`
// walk was already hand-copied TWICE (once in ExprTypecheck.cpp for CaseStmt,
// once in ExprTypecheck_Exprs.cpp for CaseExpr — byte-for-byte identical
// except for the expression-vs-statement plumbing around it). Rather than
// hand-copying it a THIRD and FOURTH time into the two choice branches, the
// walk is now the ONE shared `ExprTC::bindNestedDestructuringNames` method
// (declared in ExprTypecheck_impl.hpp, implemented once in
// ExprTypecheck.cpp — the same shape as the #3692
// checkOrderingPatternOverNullableType precedent it sits beside), called
// from all FOUR sites: CaseExpr x CaseStmt, record x choice payload. Net
// effect: 2 hand-copies -> 0 hand-copies, 1 shared implementation, while
// gaining the missing choice-side capability.
//
// §35.5 "Case patterns": "A payload case is matched by a positional
// destructuring pattern `CaseName(slot, …)` … All §19.3 slot rules carry
// over: naked identifiers bind, `var x` binds explicitly, `_` discards per
// slot, per-slot `|` alternatives among binding-free members, nested
// patterns recurse." Recursing a nested pattern that itself BINDS is exactly
// this rule; #3902 covers the CHECK half, this issue the BIND half.
//
// Ownership (§35.5 "The borrowing match"): an unmarked (default) scrutinee
// read is a borrowing match — "for a value-like choice, payload bindings
// are copies … for borrow-bearing and owning choices, class-typed payload
// bindings are borrows." A nested binding reuses the SAME codegen extraction
// path as record-in-record nesting (`patternMatches`'s record-scrutinee
// DestructuringPattern branch, LlvmBackend_Exprs2.cpp) regardless of
// whether the OUTER container is a record or a choice payload slot — the
// extracted field value is copied into a fresh alloca with no retain/move
// primitive invoked, exactly as the pre-existing `Outer(Inner(v))`
// record-in-record case does. The
// `spec_35_5_nested_bind_borrow_bearing_choice_reads_without_consuming` row
// below proves this empirically for a BORROW-BEARING choice (a choice case
// payload record — `Box` — holding a class-typed field): the nested binding
// reads the class instance's value correctly, and the ORIGINAL owning local
// (`it`) remains fully usable after the `case` completes — which could only
// compile and produce the correct sum if the nested bind is a non-consuming
// borrow, not a move (a move would either be a compile error reading `it`
// again, or a use-after-move fault). An OWNING choice's nested-binding
// transfer semantics are explicitly out of scope here: the adjacent,
// unmodified comment on the choice branch ("the owning-match transfer
// machinery lands in a follow-up") predates this fix and says the top-level
// owning-match transfer itself is still a pending follow-up slice, so
// testing a nested bind through it would be testing an unrelated,
// not-yet-implemented feature rather than this fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Wrap a method body in the standard module+decls+class shell and run it;
// return the exit code (-999 if the run failed). Mirrors
// choice_slot_pattern_check_3902_test.cpp's `runDecls`.
int runDecls(const std::string& decls, const std::string& body) {
    auto r = runAot(
        std::string("module M; ") + decls +
        std::string(" class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

const char kOuterInnerDecl[] =
    "record Inner(V: int); "
    "choice Outer Wrap(inner: Inner), None; end Outer;";

}  // namespace

// --- §35.5 / §19.3: the issue's own two-level repro -------------------------

// #3909 primary repro (issue text, verbatim shape): `Wrap(Inner(v)) => v`
// over `Outer.Wrap(new Inner(9))` must resolve `v` and return 9. Before the
// fix this was a compile-time "unresolved identifier `v`" rejection — this
// row is FAIL-WITHOUT (does not compile) / PASS-WITH (returns 9).
BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_two_level_nested_bind_naked_matches_issue_repro) {
    auto r = runDecls(kOuterInnerDecl,
        "var o := Outer.Wrap(new Inner(9)); "
        "return (case o of Wrap(Inner(v)) => v, None => 0, _ => -1);");
    BEZEL_EXPECT_EQ(r, 9);
}

// Same shape, a DIFFERENT payload value — proves `v` actually carries the
// extracted field's runtime value (not a hardcoded/misindexed constant): a
// fix that bound the wrong slot, or bound a stale/garbage value, would not
// track this change (§ lane rule: a row that passes is not evidence until
// something has shown it can fail — this is the value-sensitivity analogue
// of #3902's match/no-match pairs, since #3909's bug is a compile-time
// rejection rather than a wrong boolean).
BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_two_level_nested_bind_naked_tracks_different_value) {
    auto r = runDecls(kOuterInnerDecl,
        "var o := Outer.Wrap(new Inner(42)); "
        "return (case o of Wrap(Inner(v)) => v, None => 0, _ => -1);");
    BEZEL_EXPECT_EQ(r, 42);
}

// --- §35.5 / §19.3: the `var x` spelling of a nested slot binding -----------

// §19.3 "`var x` binds explicitly" carries over one level down: `Wrap(Inner(
// var v))` must bind identically to the naked form above.
BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_two_level_nested_bind_var_form) {
    auto r = runDecls(kOuterInnerDecl,
        "var o := Outer.Wrap(new Inner(17)); "
        "return (case o of Wrap(Inner(var v)) => v, None => 0, _ => -1);");
    BEZEL_EXPECT_EQ(r, 17);
}

// --- §35.5 / §19.3: three levels of nesting ---------------------------------

// The brief asks for at least two nesting depths; this is the third level
// (`Wrap(Mid(Deep(w)))`) — the shared walk's worklist recursion (`nestedWork`
// pushes a `deeper` DestructuringPattern back onto itself) must not stop at
// depth one.
BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_three_level_nested_bind) {
    auto r = runDecls(
        "record Deep(W: int); "
        "record Mid(D: Deep); "
        "choice Outer Wrap(mid: Mid), None; end Outer;",
        "var o := Outer.Wrap(new Mid(new Deep(100))); "
        "return (case o of Wrap(Mid(Deep(w))) => w, None => 0, _ => -1);");
    BEZEL_EXPECT_EQ(r, 100);
}

// --- §35.5 / §19.3: two names bound from ONE nested record ------------------

// `Wrap(Pair(a, b))` binds two names from the same nested destructuring —
// checks the per-index field-type lookup (`ndFields[i]`) is not off-by-one.
// Two rows with different (a, b) pairs whose sums are each unique to that
// row prove both fields are read correctly, not just their sum by
// coincidence.
BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_nested_bind_multi_field_low_values) {
    auto r = runDecls(
        "record Pair(A: int, B: int); "
        "choice Outer Wrap(p: Pair), None; end Outer;",
        "var o := Outer.Wrap(new Pair(3, 4)); "
        "return (case o of Wrap(Pair(a, b)) => a + b, None => 0, _ => -1);");
    BEZEL_EXPECT_EQ(r, 7);
}

BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_nested_bind_multi_field_high_values) {
    auto r = runDecls(
        "record Pair(A: int, B: int); "
        "choice Outer Wrap(p: Pair), None; end Outer;",
        "var o := Outer.Wrap(new Pair(10, 20)); "
        "return (case o of Wrap(Pair(a, b)) => a + b, None => 0, _ => -1);");
    BEZEL_EXPECT_EQ(r, 30);
}

// --- §35.5 / §19.3: a nested bind alongside a sibling TOP-LEVEL binding -----

// `Two(a, Inner(v))` — slot 0 is an ordinary top-level naked binding, slot 1
// is the nested destructure. Both must land in the SAME armScope without
// clobbering each other. Two rows with different (a, v) pairs whose sums
// are each unique prove both bindings independently, not just their sum.
BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_nested_bind_alongside_sibling_top_level_slot_a) {
    auto r = runDecls(
        "record Inner(V: int); "
        "choice Outer Two(a: int, inner: Inner), None; end Outer;",
        "var o := Outer.Two(100, new Inner(5)); "
        "return (case o of Two(a, Inner(v)) => a + v, None => 0, _ => -1);");
    BEZEL_EXPECT_EQ(r, 105);
}

BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_nested_bind_alongside_sibling_top_level_slot_b) {
    auto r = runDecls(
        "record Inner(V: int); "
        "choice Outer Two(a: int, inner: Inner), None; end Outer;",
        "var o := Outer.Two(1, new Inner(200)); "
        "return (case o of Two(a, Inner(v)) => a + v, None => 0, _ => -1);");
    BEZEL_EXPECT_EQ(r, 201);
}

// --- §35.5 / §21.7: the STATEMENT-position form (CaseStmt) ------------------

// The bug and fix live in TWO places — CaseExpr (ExprTypecheck_Exprs.cpp) and
// CaseStmt (ExprTypecheck.cpp). Every row above exercises the CaseExpr path
// (an expression-position `(case … of …)`); this row exercises the
// STATEMENT-position `case … of … end;` form, assigning into a pre-declared
// local from each arm's `do … end` body.
BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_statement_position_nested_bind) {
    auto r = runDecls(kOuterInnerDecl,
        "var o := Outer.Wrap(new Inner(23)); "
        "var result := 0; "
        "case o of "
        "  Wrap(Inner(v)) => do result := v; end, "
        "  None => do result := 0; end, "
        "  _ => do result := -1; end "
        "end; "
        "return result;");
    BEZEL_EXPECT_EQ(r, 23);
}

// --- Control: the #3902 pure-CHECK form (no binding) must still behave -----

// §3902 fixed the codegen-side CHECK for a nested-destructuring payload
// slot; this fix touches only the sema-side BINDING path (a fresh call to
// bindNestedDestructuringNames, which does nothing when a nested slot's own
// sub-patterns contain no naked/`var` binding). These two rows mirror
// choice_slot_pattern_check_3902_test.cpp's nested-check rows to confirm
// that control is unaffected by this change.
BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_control_pure_check_mismatch_still_falls_through) {
    auto r = runDecls(kOuterInnerDecl,
        "var o := Outer.Wrap(new Inner(9)); "
        "return (case o of Wrap(Inner(5)) => 7, None => 0, _ => 3);");
    BEZEL_EXPECT_EQ(r, 3);
}

BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_control_pure_check_match_still_selects_arm) {
    auto r = runDecls(kOuterInnerDecl,
        "var o := Outer.Wrap(new Inner(5)); "
        "return (case o of Wrap(Inner(5)) => 7, None => 0, _ => 3);");
    BEZEL_EXPECT_EQ(r, 7);
}

// --- Ownership: a BORROW-BEARING choice's nested bind does not consume -----

// `Item` is a class (a reference type); `Box` is a record holding an `Item`
// field, which per §9.7 makes `Box` a BORROW-BEARING record, which per
// §35.4 rule 2 makes `Container` a BORROW-BEARING choice. Per §35.5 "The
// borrowing match" (the default, unmarked scrutinee read): "for borrow-
// bearing and owning choices, class-typed payload bindings are borrows."
// `Filled(Box(item))` binds `item` through TWO levels (choice payload slot
// -> record field) — the same codegen extraction path record-in-record
// nesting already uses, so ownership here is governed by the identical,
// already-proven rule, not a new one.
//
// The proof that `item` is a BORROW, not a transfer: `it` (the ORIGINAL
// owning local passed into `Box`) is read again AFTER the `case` completes.
// If the nested bind had somehow moved or consumed `it`'s value, reading
// `it.Value` a second time would either be rejected at compile time (a
// read of a moved-from/disposed local) or fault at runtime under the
// checked-mode disposed-flag check (§28.3) — neither happens: the program
// compiles, runs, and returns the sum of BOTH reads (7 + 7 = 14), proving
// `item` borrowed `it`'s value without disturbing `it`'s own ownership.
BEZEL_TEST(codegen_choice_nested_bind_3909,
           spec_35_5_nested_bind_borrow_bearing_choice_reads_without_consuming) {
    auto r = runDecls(
        "public class Item "
        "  public readonly property Value: int; "
        "  public constructor(v: int)  Value := v;  end constructor; "
        "end Item; "
        "record Box(Ref: Item); "
        "choice Container Filled(box: Box), Empty; end Container;",
        "var it := new Item(7); "
        "var b := new Box(it); "
        "var c := Container.Filled(b); "
        "var viaMatch := 0; "
        "case c of "
        "  Filled(Box(item)) => do viaMatch := item.Value; end, "
        "  Empty => do viaMatch := -1; end "
        "end; "
        "return viaMatch + it.Value;");
    BEZEL_EXPECT_EQ(r, 14);
}
