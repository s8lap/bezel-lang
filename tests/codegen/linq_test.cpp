// Codegen tests for LINQ operations on Array<T>.
// Covers Any/All/Count/Where/ForEach/Sum/Min/Max/First/Select/Skip/Take/
// OrderBy/OrderByDescending/Distinct/IndexOf/Contains.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// --- Any / All ---

BEZEL_TEST(linq, any_all_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return int(a.Any((x) => x > 2) and a.All((x) => x > 0) and "
        "             not a.Any((x) => x > 10)); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq, any_all_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"hello\"); a.Add(\"hi\"); "
        "  return int(a.Any((s) => s.Length > 4) and a.All((s) => s.Length > 0)); "
        "end Main; end C;"), 1);
}

// --- Count / Where ---

BEZEL_TEST(linq, count_where) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  using var evens: Array<int> := a.Where((x) => x mod 2 = 0); "
        "  return int(a.Count((x) => x mod 2 = 0) = 2 and evens.Length = 2 "
        "    and evens.Get(0) = 2 and evens.Get(1) = 4); "
        "end Main; end C;"), 1);
}

// --- Sum / Min / Max ---

BEZEL_TEST(linq, sum_min_max) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(4); a.Add(1); a.Add(5); "
        "  var s := a.Sum(); var mn := a.Min(); var mx := a.Max(); "
        "  var ss := a.Sum((x: int) => x * x); "
        "  return int(s = 14 and mn = 1 and mx = 5 and ss = 52); "
        "end Main; end C;"), 1);
}

// --- First ---

BEZEL_TEST(linq, first) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(5); a.Add(3); a.Add(7); "
        "  return a.First((x) => x > 4); "
        "end Main; end C;"), 5);
}

// --- Skip / Take ---

BEZEL_TEST(linq, skip_take) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  using var t: Array<int> := a.Take(3); "
        "  using var s: Array<int> := a.Skip(2); "
        "  using var m: Array<int> := a.Take(4).Skip(1); "
        "  return int(t.Length = 3 and t.Get(2) = 3 and "
        "             s.Length = 3 and s.Get(0) = 3 and "
        "             m.Length = 3 and m.Get(0) = 2); "
        "end Main; end C;"), 1);
}

// --- OrderBy / OrderByDescending ---

BEZEL_TEST(linq, orderby_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(5); a.Add(2); a.Add(8); a.Add(1); "
        "  using var asc: Array<int> := a.OrderBy((x: int) => x); "
        "  using var desc: Array<int> := a.OrderByDescending((x: int) => x); "
        "  return int(asc.Get(0) = 1 and asc.Get(3) = 8 and "
        "             desc.Get(0) = 8 and desc.Get(3) = 1); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq, orderby_string_by_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"cherry\"); a.Add(\"a\"); a.Add(\"bb\"); "
        "  using var sorted: Array<string> := a.OrderBy((s) => s.Length); "
        "  return int(sorted.Get(0) = \"a\" and sorted.Get(2) = \"cherry\"); "
        "end Main; end C;"), 1);
}

// --- Distinct ---

BEZEL_TEST(linq, distinct_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(1); a.Add(3); a.Add(2); "
        "  using var d: Array<int> := a.Distinct(); "
        "  return d.Length; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(linq, distinct_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"apple\"); a.Add(\"banana\"); a.Add(\"apple\"); "
        "  using var d: Array<string> := a.Distinct(); "
        "  return d.Length; "
        "end Main; end C;"), 2);
}

// --- IndexOf / Contains ---

BEZEL_TEST(linq, indexof_contains) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var i := a.IndexOf(20); var j := a.IndexOf(99); "
        "  return int(i = 1 and j = -1 and a.Contains(10) and not a.Contains(99)); "
        "end Main; end C;"), 1);
}

// --- Chaining ---

BEZEL_TEST(linq, chaining) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  for var i in 1..10 do a.Add(i); end; "
        "  using var r: Array<int> := a.Where((x) => x mod 2 = 0).OrderBy((x: int) => x).Skip(1).Take(3); "
        "  return int(r.Length = 3 and r.Get(0) = 4 and r.Get(2) = 8); "
        "end Main; end C;"), 1);
}

// --- ForEach with side effect ---

BEZEL_TEST(linq, foreach_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Sum(ref acc: int, x: int) acc := acc + x; end Sum; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    using var doubled: Array<int> := a.Select((x: int) => x * 2); "
        "    return int(doubled.Get(0) = 2 and doubled.Get(2) = 6); "
        "  end Main; end C;"), 1);
}
