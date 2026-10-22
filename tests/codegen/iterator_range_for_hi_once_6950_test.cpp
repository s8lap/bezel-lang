// Spec-derived regression test for issue #6950.
//
// Spec §29.2 (v0.340): `for` desugars to a single `GetEnumerator` over a source
// evaluated once. §16.3: `Range<T>` exposes readonly `Start`/`End`;
// `GetEnumerator` yields from them. §16.5: a counted-loop lowering is allowed
// "provided the observable behavior is identical". So the upper bound `hi` of
// `for var i in lo..<hi` is evaluated exactly once, whether the loop is in a
// plain method or in an iterator method (§29.3).
//
// THE DEFECT. In an iterator, `hi` was re-lowered in every `for.cond`: its
// side effects repeated, and a body mutating a local used in `hi` changed the
// bound (`0,1` instead of `0,1,2`).
//
// Oracle: `runAotChecked`, exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}
}  // namespace

// §29.2 / §16.3: a side-effecting `hi` runs once (iterator).
BEZEL_TEST(codegen_iterator_range_for_hi_once_6950, side_effecting_hi_prints_once) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method H(): int Console.WriteLine(\"eval\"); return 3; end H; "
            " public static method G(): IEnumerable<int> "
            "   for var i in 0..<H() do yield return i; end; end G; "
            " public static method Main() "
            "   for var v in G() do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("eval\n0\n1\n2\n"));
}

// Same, with a constant step and with a runtime step (#6904 slot).
BEZEL_TEST(codegen_iterator_range_for_hi_once_6950, side_effecting_hi_with_steps) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method H(): int Console.WriteLine(\"eval\"); return 7; end H; "
            " public static method G(): IEnumerable<int> "
            "   for var i in 0..<H() step 3 do yield return i; end; end G; "
            " public static method R(st: long): IEnumerable<int> "
            "   for var i in 0..<H() step st do yield return i; end; end R; "
            " public static method Main() "
            "   for var v in G() do Console.WriteLine(v); end; "
            "   for var v in R(3L) do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("eval\n0\n3\n6\neval\n0\n3\n6\n"));
}

// The issue's repro: the body mutates a local used in `hi`; the bound is the
// value at the range expression.
BEZEL_TEST(codegen_iterator_range_for_hi_once_6950, body_mutating_hi_local) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method G(): IEnumerable<int> "
            "   var n := 3; "
            "   for var i in 0..<n do n := n - 1; yield return i; end; end G; "
            " public static method P1(): IEnumerable<int> "
            "   var n := 3; "
            "   for var i in 0..n do n := n - 1; yield return i; end; end P1; "
            " public static method Main() "
            "   for var v in G() do Console.WriteLine(v); end; "
            "   for var v in P1() do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("0\n1\n2\n0\n1\n2\n3\n"));
}

// Control: the plain method.
BEZEL_TEST(codegen_iterator_range_for_hi_once_6950, plain_method_control) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method H(): int Console.WriteLine(\"eval\"); return 3; end H; "
            " public static method Main() "
            "   var n := 3; "
            "   for var i in 0..<n do n := n - 1; Console.WriteLine(i); end; "
            "   for var j in 0..<H() do Console.WriteLine(j); end; "
            " end Main; end Program;"),
        std::string("0\n1\n2\neval\n0\n1\n2\n"));
}

// Descending range with a computed hi that changes across yields.
BEZEL_TEST(codegen_iterator_range_for_hi_once_6950, descending_negative_step) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method G(): IEnumerable<int> "
            "   var lo := 0; "
            "   for var i in 6..lo step -2 do lo := 100; yield return i; end; end G; "
            " public static method Main() "
            "   for var v in G() do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("6\n4\n2\n0\n"));
}
