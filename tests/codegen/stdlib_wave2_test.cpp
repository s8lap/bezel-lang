// Codegen tests for stdlib wave-2 bug fixes: #650, #651, #653.
//
// #650 — LINQ FirstOrNull(predicate: (T)->bool): T? returned garbage instead
//         of the first matching element (predicate form was unimplemented).
//         Also covers: SingleOrNull() with 0/1/2 elements (no predicate form
//         in §5 spec).
//         Spec ref: stdlib §5 "Element access", v0.134.
//
// #651 — LINQ GroupBy<TKey>(key: (T)->TKey): IEnumerable<IGrouping<TKey,T>>
//         and OfType<U>(): IEnumerable<U> produced empty results because
//         neither had codegen or sema handlers.
//         Spec ref: stdlib §5 "Grouping" / "Filtering", v0.134.
//
// #653 — Weak<T>.TryUpgrade(out owned s: Shared<T>?): bool wrote back the
//         new strong handle when the call-site used "out using var" (inline
//         form), but Shared<T>? methods (Clone/Get/Downgrade) silently returned
//         nullptr when the receiver was any non-identifier expression (e.g. a
//         PostfixBangExpr such as `nullable!`).  Root cause: IdentifierExpr
//         guard in tryLowerBuiltinClassCall returned null before reaching the
//         Shared/Weak dispatch.  Additionally, "out pre_declared_var" is
//         ill-formed per spec §22.4 for out-owned parameters; sema must reject.
//         Spec refs: spec §22.4, §28.13, stdlib §2.16, v0.134.
//
// All tests use the JIT harness (runJit/runJitChecked) and return int exit codes.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runJitChecked;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

bool hasSemaError(const std::string& program) {
    auto r = runAot(program);
    return !r.ran && r.diagnostics.hasErrors();
}

// Thin prog() wrapper: wraps a main body in module+class+Main boilerplate.
std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

} // namespace

// ===========================================================================
// #650 — FirstOrNull(predicate) / SingleOrNull() — stdlib §5
// ===========================================================================

// stdlib §5: FirstOrNull() on an empty array returns null (T? with hasValue=0).
// The no-predicate form was already implemented; regression guard.
BEZEL_TEST(codegen_stdlib_650_firstornull_pred, firstornull_no_pred_empty_returns_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "var r := a.FirstOrNull(); "
        "if r = null then return 0; end; "
        "return 1; ")), 0);
}

// stdlib §5: FirstOrNull() on a non-empty array returns the first element.
BEZEL_TEST(codegen_stdlib_650_firstornull_pred, firstornull_no_pred_nonempty_returns_first) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(10); a.Add(20); a.Add(30); "
        "var r := a.FirstOrNull(); "
        "if r <> null then return r!; end; "
        "return -1; ")), 10);
}

// stdlib §5: FirstOrNull(pred) — no element satisfies predicate → null.
BEZEL_TEST(codegen_stdlib_650_firstornull_pred, firstornull_pred_no_match_returns_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(1); a.Add(2); a.Add(3); "
        "var r := a.FirstOrNull(lambda(x: int) => x > 100); "
        "if r = null then return 0; end; "
        "return 1; ")), 0);
}

// stdlib §5: FirstOrNull(pred) — first matching element is returned.
BEZEL_TEST(codegen_stdlib_650_firstornull_pred, firstornull_pred_match_returns_first) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(5); a.Add(15); a.Add(25); "
        "var r := a.FirstOrNull(lambda(x: int) => x > 10); "
        "if r <> null then return r!; end; "
        "return -1; ")), 15);
}

// stdlib §5: FirstOrNull(pred) on empty array → null.
BEZEL_TEST(codegen_stdlib_650_firstornull_pred, firstornull_pred_empty_returns_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "var r := a.FirstOrNull(lambda(x: int) => x > 0); "
        "if r = null then return 0; end; "
        "return 1; ")), 0);
}

// stdlib §5: FirstOrNull(pred) returns the first (not any) match when multiple
// elements satisfy the predicate.
BEZEL_TEST(codegen_stdlib_650_firstornull_pred, firstornull_pred_returns_first_not_last) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(3); a.Add(7); a.Add(11); a.Add(19); "
        "var r := a.FirstOrNull(lambda(x: int) => x > 5); "
        "if r <> null then return r!; end; "
        "return -1; ")), 7);
}

// stdlib §5: SingleOrNull() — empty array → null.
BEZEL_TEST(codegen_stdlib_650_firstornull_pred, singleornull_empty_returns_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "var r := a.SingleOrNull(); "
        "if r = null then return 0; end; "
        "return 1; ")), 0);
}

// stdlib §5: SingleOrNull() — exactly one element → that element.
BEZEL_TEST(codegen_stdlib_650_firstornull_pred, singleornull_one_element_returns_it) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(42); "
        "var r := a.SingleOrNull(); "
        "if r <> null then return r!; end; "
        "return -1; ")), 42);
}

// stdlib §5: SingleOrNull() — more than one element → null (not exactly one).
BEZEL_TEST(codegen_stdlib_650_firstornull_pred, singleornull_two_elements_returns_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(1); a.Add(2); "
        "var r := a.SingleOrNull(); "
        "if r = null then return 0; end; "
        "return 1; ")), 0);
}

// stdlib §5: FirstOrNull(pred) chained with Where — predicate on a lazy source.
BEZEL_TEST(codegen_stdlib_650_firstornull_pred, firstornull_pred_on_lazy_source) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(2); a.Add(4); a.Add(6); a.Add(8); "
        "var r := a.Where(lambda(x: int) => x > 4).FirstOrNull(lambda(x: int) => x > 5); "
        "if r <> null then return r!; end; "
        "return -1; ")), 6);
}

// ===========================================================================
// #661 — SingleOrNull(predicate) — stdlib §5
// ===========================================================================
// Per stdlib §5: "the *OrNull variants return null instead [of throwing]" and
// Single() "throws if not exactly one".  SingleOrNull(pred) returns the unique
// element satisfying the predicate, or null if zero or two+ match.

// stdlib §5 (#661): SingleOrNull(pred) — no element matches → null.
BEZEL_TEST(codegen_stdlib_661_singleornull_pred, singleornull_pred_no_match_returns_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(10); a.Add(20); a.Add(30); "
        "var r := a.SingleOrNull(lambda(x: int) => x > 100); "
        "if r = null then return 0; end; "
        "return 1; ")), 0);
}

// stdlib §5 (#661): SingleOrNull(pred) — exactly one match → that element.
BEZEL_TEST(codegen_stdlib_661_singleornull_pred, singleornull_pred_one_match_returns_it) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(10); a.Add(20); a.Add(30); "
        "var r := a.SingleOrNull(lambda(x: int) => x = 20); "
        "if r <> null then return r!; end; "
        "return -1; ")), 20);
}

// stdlib §5 (#661): SingleOrNull(pred) — two matches → null (not unique).
BEZEL_TEST(codegen_stdlib_661_singleornull_pred, singleornull_pred_two_matches_returns_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(10); a.Add(20); a.Add(20); a.Add(30); "
        "var r := a.SingleOrNull(lambda(x: int) => x = 20); "
        "if r = null then return 0; end; "
        "return 1; ")), 0);
}

// stdlib §5 (#661): SingleOrNull(pred) — empty source → null.
BEZEL_TEST(codegen_stdlib_661_singleornull_pred, singleornull_pred_empty_returns_null) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "var r := a.SingleOrNull(lambda(x: int) => x > 0); "
        "if r = null then return 0; end; "
        "return 1; ")), 0);
}

// stdlib §5 (#661): SingleOrNull(pred) — scans all elements (does not short-circuit).
// Three elements, only element at index 2 matches — must scan to the end.
BEZEL_TEST(codegen_stdlib_661_singleornull_pred, singleornull_pred_match_at_end) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(1); a.Add(2); a.Add(99); "
        "var r := a.SingleOrNull(lambda(x: int) => x = 99); "
        "if r <> null then return r!; end; "
        "return -1; ")), 99);
}

// stdlib §5 (#661): probe_071b verification — the original failing probe.
// a=[10,20,30]; SingleOrNull(x=20) → 20.
BEZEL_TEST(codegen_stdlib_661_singleornull_pred, singleornull_pred_probe_071b_case) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(10); a.Add(20); a.Add(30); "
        "var f: int? := a.FirstOrNull(lambda(x: int) => x > 15); "
        "var l: int? := a.LastOrNull(); "
        "var s: int? := a.SingleOrNull(lambda(x: int) => x = 20); "
        "if f! = 20 and l! = 30 and s! = 20 then return 1; end; "
        "return 0; ")), 1);
}

// ===========================================================================
// #651 — GroupBy / OfType — stdlib §5
// ===========================================================================

// fix #3559 (stdlib §5, v0.285): the three `OfType<int>()` rows that stood
// here were REMOVED with the member. v0.284 (proposal 0121 R2) withdrew
// `OfType<U>()` from stdlib §5, so "returns all elements", "returns empty" and
// "chains with Where" are no longer claims the standard library makes — the
// operator does not exist to make them about. `GroupBy` below is unaffected;
// it was the other half of #651 and stays.
//
// The rejection regression tests for the removal, including the identity cell
// these three used (which the issue-3440 interim still ADMITTED, so it is one
// of the rows that can only pass on a compiler carrying #3559), live in
// `tests/sema/stdlib_v0284_removals_3559_test.cpp`.

// stdlib §5 GroupBy: GroupBy on empty array → empty result.
BEZEL_TEST(codegen_stdlib_651_groupby_oftype, groupby_empty_source_returns_empty) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "using var groups := a.GroupBy(lambda(x: int) => x).ToArray(); "
        "return groups.Count; ")), 0);
}

// stdlib §5 GroupBy: {1,2,1} grouped by identity → 2 groups (keys 1 and 2).
// stdlib §5 / IGrouping: each group's Key is the key value.
BEZEL_TEST(codegen_stdlib_651_groupby_oftype, groupby_int_identity_two_distinct_keys) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(1); a.Add(2); a.Add(1); "
        "using var groups := a.GroupBy(lambda(x: int) => x).ToArray(); "
        "return groups.Count; ")), 2);
}

// stdlib §5 GroupBy: {10,20,10,30} grouped by identity →
// 3 groups; sum of group sizes = 4 (the original element count).
BEZEL_TEST(codegen_stdlib_651_groupby_oftype, groupby_group_element_count) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(10); a.Add(20); a.Add(10); a.Add(30); "
        "using var groups := a.GroupBy(lambda(x: int) => x).ToArray(); "
        "if groups.Count <> 3 then return -1; end; "
        "var total := 0; "
        "var i := 0; "
        "while i < groups.Count do "
        "  total := total + groups[i].Count(); "
        "  i := i + 1; "
        "end; "
        "return total; ")), 4);
}

// stdlib §5 GroupBy: IGrouping<TKey, T>.Key returns the correct key value.
BEZEL_TEST(codegen_stdlib_651_groupby_oftype, groupby_key_property_correct) {
    // {5, 5, 5} grouped by identity → 1 group, Key = 5.
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(5); a.Add(5); a.Add(5); "
        "using var groups := a.GroupBy(lambda(x: int) => x).ToArray(); "
        "if groups.Count <> 1 then return -1; end; "
        "return groups[0].Key; ")), 5);
}

// #752 — a BARE GroupBy(...) (no .ToArray()) must compose with the generic lazy
// operators and for-in, not only the special-cased .ToArray() path. Before the
// fix, GroupBy lowered to garbage for every consumer except ToArray (Count → 0
// /garbage, Any → false, for-in → "not yet implemented"). lowerToLazyEnumerator
// now materializes GroupBy into a lazy ARRAY source over its IGrouping elements.
BEZEL_TEST(codegen_stdlib_752_groupby_lazy, groupby_count_composes) {
    // {1,2,3,4} grouped by mod 2 → 2 groups (keys 1, 0).
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "return a.GroupBy(lambda(x: int) => x mod 2).Count(); ")), 2);
}
BEZEL_TEST(codegen_stdlib_752_groupby_lazy, groupby_any_composes) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(1); a.Add(2); "
        "if a.GroupBy(lambda(x: int) => x mod 2).Any() then return 1; else return 0; end; ")),
        1);
}
BEZEL_TEST(codegen_stdlib_752_groupby_lazy, groupby_any_empty_is_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "if a.GroupBy(lambda(x: int) => x mod 2).Any() then return 1; else return 0; end; ")),
        0);
}
BEZEL_TEST(codegen_stdlib_752_groupby_lazy, groupby_for_in_sums_group_counts) {
    // {1..6} grouped by mod 3 → 3 groups, each with 2 elements → sum of counts 6.
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); a.Add(6); "
        "var total := 0; "
        "for var g in a.GroupBy(lambda(x: int) => x mod 3) do total := total + g.Count(); end; "
        "return total; ")), 6);
}
BEZEL_TEST(codegen_stdlib_752_groupby_lazy, groupby_for_in_keys_correct) {
    // {1..6} grouped by mod 3 → keys {1,2,0} → sum 3 (proves Key per group is right
    // when iterated through the lazy enumerator, not just the .ToArray() path).
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<int>(); "
        "a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); a.Add(6); "
        "var keysum := 0; "
        "for var g in a.GroupBy(lambda(x: int) => x mod 3) do keysum := keysum + g.Key; end; "
        "return keysum; ")), 3);
}


// ===========================================================================
// #653 — Shared<T>/Weak<T> with nullable receiver and TryUpgrade — §28.13
// ===========================================================================

// §28.13 / stdlib §2.16: Clone() on a nullable Shared<T>? via PostfixBangExpr.
// `h!.Clone()` must lower correctly — the receiver is PostfixBangExpr, not a
// bare IdentifierExpr.
BEZEL_TEST(codegen_shared_weak_653, shared_clone_via_nullable_bang_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Obj public Val: int := 0; end Obj; "
        "class C "
        "  public static method Main(): int "
        "    var obj := new Obj(); obj.Val := 42; "
        "    using var h: Shared<Obj>? := new Shared<Obj>(owned obj); "
        "    using var h2 := h!.Clone(); "
        "    if h2.Get().Val = 42 then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 0);
}

// §28.13 / stdlib §2.16: Get() on a nullable Shared<T>? via PostfixBangExpr.
BEZEL_TEST(codegen_shared_weak_653, shared_get_via_nullable_bang_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Obj public Val: int := 0; end Obj; "
        "class C "
        "  public static method Main(): int "
        "    var obj := new Obj(); obj.Val := 99; "
        "    using var h: Shared<Obj>? := new Shared<Obj>(owned obj); "
        "    var inner := h!.Get(); "
        "    if inner.Val = 99 then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 0);
}

// §28.13 / stdlib §2.16: Downgrade() on a nullable Shared<T>? via PostfixBangExpr.
BEZEL_TEST(codegen_shared_weak_653, shared_downgrade_via_nullable_bang_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Obj public Val: int := 0; end Obj; "
        "class C "
        "  public static method Main(): int "
        "    var obj := new Obj(); obj.Val := 7; "
        "    using var h: Shared<Obj>? := new Shared<Obj>(owned obj); "
        "    using var w := h!.Downgrade(); "
        "    if w.TryUpgrade(out using var h2) then "
        "      if h2.Get().Val = 7 then return 0; end; "
        "      return 1; "
        "    end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 0);
}

// §28.13 / spec §22.4: TryUpgrade with "out using var" (inline form) works —
// the new strong handle is written, GetVal reads through it correctly.
BEZEL_TEST(codegen_shared_weak_653, tryupgrade_inline_out_using_var_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Obj public Val: int := 0; end Obj; "
        "class C "
        "  public static method Main(): int "
        "    var obj := new Obj(); obj.Val := 55; "
        "    using var h := new Shared<Obj>(owned obj); "
        "    using var w := h.Downgrade(); "
        "    if w.TryUpgrade(out using var h2) then "
        "      if h2.Get().Val = 55 then return 0; end; "
        "      return 1; "
        "    end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 0);
}

// spec §22.4 / §28.13: "out pre_declared_var" is ill-formed at call site for
// an "out owned" parameter — sema must reject it with a diagnostic error.
// TryUpgrade(out owned s: Shared<T>?): bool — calling with "out upgraded" where
// "upgraded" is a pre-declared variable is not a valid call-site form.
BEZEL_TEST(codegen_shared_weak_653, tryupgrade_out_predeclared_var_sema_error) {
    // Using "out upgraded" (pre-declared variable) must be a sema error
    // per spec §22.4: accepted forms are "out using var x" and "out owned var x"
    // only. Plain "out var x" would also be invalid for out-owned parameters.
    BEZEL_EXPECT_TRUE(hasSemaError(
        "module M; "
        "class Obj public Val: int := 0; end Obj; "
        "class C "
        "  public static method Main(): int "
        "    var obj := new Obj(); "
        "    using var shared := new Shared<Obj>(owned obj); "
        "    using var weak := shared.Downgrade(); "
        "    using var upgraded: Shared<Obj>? := null; "
        "    var ok := weak.TryUpgrade(out upgraded); "
        "    return 0; "
        "  end Main; "
        "end C;"));
}
