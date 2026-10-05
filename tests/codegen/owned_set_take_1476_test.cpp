// Codegen — fix #1476 / stdlib §4.10: `OwningSet<T>.Pop(query: T): owned T`
// and its no-throw analogue `TryPop(query: T, out owned removed: T?): bool`
// (a class-typed T, per the `where T: class` constraint stdlib §4.10 places on
// OwningSet<T>).
//
// Root cause was three-layered (all three fixed here):
//   1. sema (ExprTypecheck_Calls.cpp): `s.Pop(query)` and the generic LINQ
//      `Pop(count: int)` operator share both a method name AND an arg count
//      (one positional arg), so `Pop` on an OwningSet receiver fell into the
//      `typeLinqOperator` fallback and was mistyped as a lazy IEnumerable<T>.
//      Codegen's upstream LINQ Pop(count) dispatcher then treated the
//      class-pointer query as an int count — `sext ptr ... to i64`, malformed
//      IR LLVM rejects (the exact #1476 crash). Fixing the first (correctly
//      scoped, `colName == "OwningSet"`) typing arm was not enough on its
//      own: `typeLinqOperator` is invoked AGAIN, unconditionally, from a
//      second if/else-if chain further down in the same function (broken
//      from the first by two intervening standalone `if` statements for
//      Array.Sort / OwningArray.Reversed-Sorted) — that second chain re-ran
//      regardless of what the first chain decided and clobbered the typing
//      right back to a lazy IEnumerable<T>. The fix excludes
//      `m == "Pop" && colName == "OwningSet"` inside `typeLinqOperator`
//      itself, the single call site both chains share.
//   2. codegen (LlvmBackend_Calls3.cpp, tryLowerLazyLinqCall): even with the
//      sema fix, `isCollectionTypeName(OwningSet)` is true and the codegen
//      LINQ dispatcher's `isOp` unconditionally matched `m == "Pop"` —
//      gated off for an OwningSet receiver so the call falls through to a
//      new dedicated handler in LlvmBackend_Calls.cpp
//      (tryLowerCollectionInstanceCall) that scans for the equal element
//      (§8.3 equality, honouring a user Equals override) and transfers its
//      ownership out via bz_array_remove_at (no disposal — mirrors
//      Pop/Dequeue's get-then-remove shape), raising InvalidOperationException
//      on no match (mirrors OwningDictionary.PopValue's "raises if not
//      present" contract). TryPop mirrors this without throwing, writing the
//      out param (null on the false path, the #891 pre-null convention).
//   3. ownership tracking (common/OwnershipFacts.cpp, isBuiltinOwnedTransferCall
//      — the single shared predicate the sema ownership pass AND the Emitter's
//      `using var` scope-end disposal registration both consult): "Pop" on an
//      OwningSet receiver was not recognised as a builtin owned-transfer call
//      (unlike PopLast/PopFirst/PopAt/PopValue), so even with (1) and (2)
//      fixed, `using var taken := s.Pop(q);` was never registered for
//      scope-end disposal — `taken` leaked, and a bare `s.Pop(q);` discard
//      statement was NOT rejected by the §21.9 leak diagnostic (a second,
//      separate hand-rolled predicate in ExprTypecheck.cpp's discard-statement
//      check needed the same addition, since it does not consult the shared
//      predicate). Both are fixed: OwningSet.Pop is now recognised as an
//      owned-transfer producer in both places (TryPop's transfer goes through
//      its `out owned` PARAMETER instead, already correctly tracked by the
//      pre-existing isOutOwnedArgPosition mechanism).
//
// Verified via standalone bezelc runs with counting destructors confirming
// exactly-once disposal (no leak, no double-free) for every scenario below
// before promoting to gtest form.

// AOT MIGRATION (JIT->AOT): runs under the AOT driver (runAot) — the stricter
// oracle (full ownership + definite-assignment pipeline). Tests whose packed
// return value exceeds 255 (the 8-bit exit-code ceiling) print it via
// Console.WriteLine and assert on stdout through runAotOut; the §21.9 bare-
// discard leak test asserts an AOT compile reject.
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Shared destructor-counter class template (mirrors owned_set_drain_1475_
// test.cpp's `R` / makeProgram helper).
std::string makeProgram(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        + mainBody + " end Main; end C;";
}

// A class with a user Equals/GetHashCode override keyed on field X — mirrors
// owned_set_union_1474_test.cpp's `Box` so Pop/TryPop's match is proven to
// honour the same §8.3 equality contract as Add/TryAdd/UnionWith, not
// reference identity.
const char* kBox =
    "module M; class Box "
    "  public static n: int := 0; "
    "  public X: int; "
    "  public constructor(x: int) X := x; n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Box then var b := other as Box; return b!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end Box; class C ";
}  // namespace

// --------------------------------------------------------------------------
// Pop(query): owned T
// --------------------------------------------------------------------------

// stdlib §4.10: Pop finds the element equal to `query` (here, `query` IS the
// stored element via s.First(), so reference identity trivially matches),
// removes it (Count decreases), and returns it — the returned Tag matches.
BEZEL_TEST(owned_set_take_1476, pop_found_returns_correct_element_and_count_spec_4_10) {
    // Packed value 3210 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(makeProgram(
        "using var s := new OwningSet<R>(); "
        "s.Add(new R(1)); s.Add(new R(2)); s.Add(new R(3)); "
        "var countBefore := s.Count; "
        "var q := s.First(); "
        "var expectedTag := q.Tag; "
        "using var taken := s.Pop(q); "
        "var countAfter := s.Count; "
        "var tagOk := 0; if taken.Tag = expectedTag then tagOk := 1; end; "
        "Console.WriteLine(countBefore * 1000 + countAfter * 100 + tagOk * 10); return 0;"
        // countBefore=3, countAfter=2, tagOk=1 => 3000 + 200 + 10 = 3210
    )), "3210");
}

// §4.10 / §28.3: the Pop'd element is NOT disposed while still held by the
// `using var` (ownership transferred, not destroyed) — verified by reading
// the destructor counter before the enclosing scope ends — and after the
// scope ends, every element (the 2 remaining in `s` + the 1 transferred to
// `taken`) is destructed EXACTLY once: no leak (would read < 3), no
// double-free (would read > 3 or crash).
BEZEL_TEST(owned_set_take_1476, pop_disposes_exactly_once_no_leak_no_double_free_spec_28_3) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "var beforeDispose := -1; "
        "do "
        "  using var s := new OwningSet<R>(); "
        "  s.Add(new R(1)); s.Add(new R(2)); s.Add(new R(3)); "
        "  var q := s.First(); "
        "  using var taken := s.Pop(q); "
        "  beforeDispose := R._n; "
        "end; "
        "return beforeDispose * 10 + R._n;"  // 0*10 + 3 = 3
    )), 3);
}

// §4.10 / §8.3: Pop matches by the element's equality contract — a user
// Equals override is honoured (content equality), not reference identity.
// `query` is a DISTINCT instance from the stored Box(5); Pop must still find
// and remove it. All three live objects (the surviving Box(7), the removed
// Box(5), and the separate query instance) are accounted for exactly once.
BEZEL_TEST(owned_set_take_1476, pop_honors_user_equals_override_spec_4_10_8_3) {
    // Packed value 215030 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(std::string(kBox) +
        "public static method Main(): int "
        "  var countBefore := 0; var countAfter := 0; var takenX := -1; "
        "  var nBefore := 0; "
        "  do "
        "    using var s := new OwningSet<Box>(); "
        "    s.Add(new Box(5)); s.Add(new Box(7)); "
        "    countBefore := s.Count; "
        "    using var query := new Box(5); "  // distinct instance, same X
        "    using var taken := s.Pop(query); "
        "    countAfter := s.Count; "
        "    takenX := taken.X; "
        "    nBefore := Box.n; "               // query + taken + surviving Box(7)
        "  end; "
        "  Console.WriteLine(countBefore * 100000 + countAfter * 10000 + takenX * 1000 + "
        "         nBefore * 10 + Box.n); return 0; "
        // 2*100000 + 1*10000 + 5*1000 + 3*10 + 0 = 215030
        "end Main; end C;"), "215030");
}

// stdlib §4.9 / §4.10: Pop raises InvalidOperationException when no equal
// element is present (mirrors OwningDictionary.PopValue's "raises if not
// present" contract) — a fresh, never-added instance is passed as the query.
// The set's existing elements are untouched: both are still disposed exactly
// once by the set's own scope-end Dispose(), and the separately-owned
// `missing` query is disposed exactly once by its own `using`.
BEZEL_TEST(owned_set_take_1476, pop_not_found_raises_and_leaves_set_intact_spec_4_9_4_10) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "var result := 0; "
        "do "
        "  using var s := new OwningSet<R>(); "
        "  s.Add(new R(1)); s.Add(new R(2)); "
        "  using var missing := new R(99); "
        "  try "
        "    using var t := s.Pop(missing); "
        "    result := 1; "  // must not reach
        "  catch InvalidOperationException => result := 2; "
        "  end; "
        "end; "
        "return result * 10 + R._n;"  // 2*10 + 3 = 23 (2 in s + 1 missing, all disposed once)
    )), 23);
}

// §4.10: Pop on an EMPTY set also raises (the not-found path, len == 0).
BEZEL_TEST(owned_set_take_1476, pop_on_empty_set_raises_spec_4_10) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "var result := 0; "
        "do "
        "  using var s := new OwningSet<R>(); "
        "  using var q := new R(1); "
        "  try "
        "    using var t := s.Pop(q); "
        "    result := 1; "  // must not reach
        "  catch InvalidOperationException => result := 2; "
        "  end; "
        "end; "
        "return result * 10 + R._n;"  // 2*10 + 1 = 21 (only q, disposed once)
    )), 21);
}

// §21.9 / #1476: `s.Pop(q);` as a bare discard statement now correctly
// leaks-diagnoses at compile time (Pop's owned-transfer shape is recognised
// by the shared common::isBuiltinOwnedTransferCall predicate the ownership
// pass consults) instead of silently compiling and leaking the removed
// element.
BEZEL_TEST(owned_set_take_1476, pop_bare_discard_is_compile_error_spec_21_9) {
    // AOT reject: the driver's front-end runs the same §21.9 leak diagnostic; a
    // rejected program yields !ran with an Error diagnostic (the driver stderr).
    auto r = runAot(makeProgram(
        "using var s := new OwningSet<R>(); "
        "s.Add(new R(1)); "
        "var q := s.First(); "
        "s.Pop(q); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(!r.ran && r.diagnostics.hasErrors());
}

// --------------------------------------------------------------------------
// TryPop(query, out owned removed): bool
// --------------------------------------------------------------------------

// stdlib §4.10: TryPop's no-throw success path — returns true, writes the
// found element through the out param (ownership transferred), and Count
// decreases. The out-param element is disposed exactly once (via the
// established `item?.Dispose()` idiom for a builtin Try*'s out-owned
// recipient — owned_collection_trypop_831_test.cpp's pattern).
BEZEL_TEST(owned_set_take_1476, trypop_found_true_transfers_and_decrements_count_spec_4_10) {
    // Packed value 1032013 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(makeProgram(
        "var ok := false; var tag := -1; "
        "var countBefore := 0; var countAfter := 0; "
        "do "
        "  using var s := new OwningSet<R>(); "
        "  s.Add(new R(1)); s.Add(new R(2)); s.Add(new R(3)); "
        "  countBefore := s.Count; "
        "  var q := s.First(); "
        "  var item: R? := null; "
        "  ok := s.TryPop(q, out item); "
        "  countAfter := s.Count; "
        "  if item <> null then tag := item.Tag; end; "
        "  item?.Dispose(); "
        "end; "
        "var okI := 0; if ok then okI := 1; end; "
        "Console.WriteLine(okI * 1000000 + countBefore * 10000 + countAfter * 1000 + "
        "       tag * 10 + R._n); return 0;"
        // 1*1000000 + 3*10000 + 2*1000 + 1*10 + 3 = 1032013
    )), "1032013");
}

// stdlib §4.10: TryPop's no-throw failure path — returns false, the out
// param is left null (the #891 pre-null convention: a nullable class
// out-param must not hold garbage), Count is unchanged, and no element is
// disturbed: both surviving elements plus the separately-owned `missing`
// query are each disposed exactly once (no leak, no double-free).
BEZEL_TEST(owned_set_take_1476, trypop_not_found_false_null_out_no_mutation_spec_4_10) {
    // Packed value 102203 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(makeProgram(
        "var ok := true; var isNull := false; "
        "var countBefore := 0; var countAfter := 0; "
        "do "
        "  using var s := new OwningSet<R>(); "
        "  s.Add(new R(1)); s.Add(new R(2)); "
        "  using var missing := new R(99); "
        "  countBefore := s.Count; "
        "  var item: R? := null; "
        "  ok := s.TryPop(missing, out item); "
        "  countAfter := s.Count; "
        "  isNull := item = null; "
        "  item?.Dispose(); "
        "end; "
        "var okI := 0; if ok then okI := 1; end; "
        "var nullI := 0; if isNull then nullI := 1; end; "
        "Console.WriteLine(okI * 1000000 + nullI * 100000 + countBefore * 1000 + "
        "       countAfter * 100 + R._n); return 0;"
        // 0 + 1*100000 + 2*1000 + 2*100 + 3 = 102203
    )), "102203");
}

// §4.10 / §8.3: TryPop also matches by the element's equality contract (user
// Equals override), not reference identity — mirrors the Pop test above.
BEZEL_TEST(owned_set_take_1476, trypop_honors_user_equals_override_spec_4_10_8_3) {
    // Packed value 1050 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(std::string(kBox) +
        "public static method Main(): int "
        "  var ok := false; var okX := -1; "
        "  do "
        "    using var s := new OwningSet<Box>(); "
        "    s.Add(new Box(5)); s.Add(new Box(7)); "
        "    using var query := new Box(5); "  // distinct instance, same X
        "    var item: Box? := null; "
        "    ok := s.TryPop(query, out item); "
        "    if item <> null then okX := item.X; end; "
        "    item?.Dispose(); "
        "  end; "
        "  var okI := 0; if ok then okI := 1; end; "
        "  Console.WriteLine(okI * 1000 + okX * 10 + Box.n); return 0; "
        // 1*1000 + 5*10 + 0 = 1050
        "end Main; end C;"), "1050");
}

// --------------------------------------------------------------------------
// Regression guard: the generic LINQ Take(count: int) operator on ordinary
// (non-OwningSet) collections is unaffected by the OwningSet-specific
// exclusion added to both tryLowerLazyLinqCall (codegen) and
// typeLinqOperator (sema).
// --------------------------------------------------------------------------

BEZEL_TEST(owned_set_take_1476, linq_take_count_on_array_unaffected_regression) {
    // 306 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3, 4, 5]; "
        "  var t := a.Take(3).ToArray(); "
        "  Console.WriteLine(t.Count * 100 + t.Sum()); return 0; "  // 3*100 + (1+2+3) = 306
        "end Main; end C;"), "306");
}

BEZEL_TEST(owned_set_take_1476, linq_take_count_on_owning_array_unaffected_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Cell public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; end Cell; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Cell>(); "
        "  a.Add(move (new Cell(1))); a.Add(move (new Cell(2))); "
        "  a.Add(move (new Cell(3))); "
        "  var sum := 0; "
        "  for var c in a.Take(2) do sum := sum + c.V; end; "
        "  return sum; "  // 1 + 2 = 3
        "end Main; end C;"), 3);
}
