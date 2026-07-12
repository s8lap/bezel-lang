// fix #774 / §23.4: indexers may be overloaded by parameter type list, and a
// call dispatches to the correct overload by the argument's type.
//
// §23.4: "Indexers may be overloaded by parameter type list." Two indexers with
// distinct parameter types (`[i: int]` and `[b: bool]`) are a legal overload
// pair. Before the fix the declaration was rejected as a "duplicate member
// `.this`" (FALSE_REJECT), because indexer PropertySymbols were not treated as
// overloadable; and the codegen/sema indexer lookup returned the FIRST indexer
// regardless of argument type, so dispatch and result typing were wrong.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
bool rejected(const std::string& program) {
    return !runAot(program).ran;
}
}  // namespace

// §23.4: an `[i: int]` and a `[b: bool]` overload (same return type) — the call
// dispatches by argument type: m[5] -> "int", m[true] -> "bool". (The repro.)
BEZEL_TEST(indexer_overload_774, dispatch_same_return_type) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class MultiIndex "
        "  public property [i: int]: string => \"int\"; "
        "  public property [b: bool]: string => \"bool\"; "
        "end MultiIndex; "
        "class C public static method Main(): int "
        "  using var m := new MultiIndex(); "
        "  return 0 if m[5] = \"int\" and m[true] = \"bool\" else 1; "
        "end Main; end C;"), 0);
}

// §23.4: overloads with DIFFERENT return types — the int overload yields int,
// the bool overload yields string. Exercises both codegen dispatch (right
// getter) and sema typing (right IndexExpr result type). m[5] -> 50 (int),
// m[true] -> "yes" (string).
BEZEL_TEST(indexer_overload_774, dispatch_different_return_types) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class MultiIndex "
        "  public property [i: int]: int => i * 10; "
        "  public property [b: bool]: string => \"yes\" if b else \"no\"; "
        "end MultiIndex; "
        "class C public static method Main(): int "
        "  using var m := new MultiIndex(); "
        "  var n := m[5]; "
        "  var s := m[true]; "
        "  return 0 if n = 50 and s = \"yes\" else 1; "
        "end Main; end C;"), 0);
}

// §23.4 regression guard: a class with a SINGLE indexer is unaffected by the
// overload-dispatch path (it falls back to the only indexer). b[5] -> 105.
BEZEL_TEST(indexer_overload_774, single_indexer_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box public property [i: int]: int => i + 100; end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  return 0 if b[5] = 105 else 1; "
        "end Main; end C;"), 0);
}

// §22.6 / §23.4: two indexers with the SAME parameter type are still an
// indistinguishable-overload error (the distinctness check now covers indexers).
BEZEL_TEST(indexer_overload_774, duplicate_param_type_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; "
        "class Dup "
        "  public property [i: int]: int => i; "
        "  public property [j: int]: int => j + 1; "
        "end Dup; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// ---------------------------------------------------------------------------
// fix #1172 / §23.4: overload resolution must use the FULL parameter list
// (arity AND every parameter type), not just the first parameter. Before the
// fix, two overloads sharing a first parameter type, or a call whose argument
// count exceeds the indexer's parameter count, were mis-resolved.
// ---------------------------------------------------------------------------

// §23.4 (#1172, repro 1 — was IR_FAIL): two 2-parameter overloads sharing the
// first parameter type — `[r:int, c:int]` and `[r:int, s:string]`. The call
// `g[3, "xy"]` must select the (int, string) overload by its FULL list, not the
// (int, int) overload by first-param match (which bound a string to an int
// parameter → `sadd.with.overflow.i32(i32, ptr)` → IR verification failure).
// 2000 + 3*10 + len("xy") = 2032. The companion int call selects (int, int):
// 1000 + 3*10 + 5 = 1035.
BEZEL_TEST(indexer_overload_1172, full_param_list_distinguishes_second_param) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Grid "
        "  public property [r: int, c: int]: int => 1000 + r*10 + c; "
        "  public property [r: int, s: string]: int => 2000 + r*10 + s.Length; "
        "end Grid; "
        "class C public static method Main(): int "
        "  using var g := new Grid(); "
        "  return 0 if g[3, \"xy\"] = 2032 and g[3, 5] = 1035 else 1; "
        "end Main; end C;"), 0);
}

// §23.4 (#1172, repro 2 — was WRONG_VALUE): a 1-parameter overload declared
// BEFORE a 2-parameter overload. `g[7, 9]` must select the 2-parameter overload
// by arity, not the leading 1-parameter overload (which dropped the second index
// and returned 107). 5000 + 7*100 + 9 = 5709; the 1-arg call g[7] = 107.
BEZEL_TEST(indexer_overload_1172, arity_selects_two_param_over_leading_one) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Grid "
        "  public property [i: int]: int => 100 + i; "
        "  public property [r: int, c: int]: int => 5000 + r*100 + c; "
        "end Grid; "
        "class C public static method Main(): int "
        "  using var g := new Grid(); "
        "  return 0 if g[7, 9] = 5709 and g[7] = 107 else 1; "
        "end Main; end C;"), 0);
}

// §23.4 (#1172, repro 3 — was ACCEPT_TRAP): a call whose argument count matches
// no indexer's parameter count is a compile error. `g[7, 9]` against a class
// declaring ONLY `[i: int]` was silently accepted (dropping the 9, returning
// 107); it must now be rejected ("no indexer overload matches the 2 arguments").
BEZEL_TEST(indexer_overload_1172, arity_mismatch_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; "
        "class Grid public property [i: int]: int => 100 + i; end Grid; "
        "class C public static method Main(): int "
        "  using var g := new Grid(); "
        "  return g[7, 9]; "
        "end Main; end C;"));
}

// §23.4 (#1172) regression guard: the set / compound / increment write-back
// paths must select the SAME multi-parameter overload as the read path. With a
// settable `[i:int]` and `[r:int,c:int]` over a `[6]int` backing, exercise
// `g[1,1] := 5` (set), `g[1,1] += 10` and `g[1,1]++` (compound + increment),
// then a 1-arg `g[0] := 7`. Final read-backs: 5 (a) + 16 (b) + 7 (d) = 28.
BEZEL_TEST(indexer_overload_1172, set_compound_incr_select_full_list) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Cell "
        "  private _d: [6]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; end; "
        "  end; "
        "  public property [r: int, c: int]: int "
        "    get => _d[r*2 + c]; "
        "    set => do _d[r*2 + c] := value; end; "
        "  end; "
        "end Cell; "
        "class C public static method Main(): int "
        "  using var g := new Cell(); "
        "  g[1, 1] := 5; "
        "  var a := g[1, 1]; "
        "  g[1, 1] += 10; "
        "  g[1, 1]++; "
        "  var b := g[1, 1]; "
        "  g[0] := 7; "
        "  var d := g[0]; "
        "  return a + b + d; "
        "end Main; end C;"), 28);
}
