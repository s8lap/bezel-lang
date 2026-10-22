// Spec-derived regression test for issue #6951.
//
// Spec §21.4 (v0.340): `for` iterates any iterable source and, with a range
// literal, visits every element; §16.5 a range may be iterated with `for`.
// §29.3: `yield return` produces the next value and suspends; the iterator is a
// state machine whose locals survive the suspend. A `for` over a range literal
// inside a block-form `case` arm of an iterator must therefore yield every
// element, exactly as it does outside a `case`.
//
// THE DEFECT. The frame-field reservation walk (collectSuspendLocals) did not
// descend into `case` arms, so the loop variable and counter of a `for` in an
// arm had no frame slot and were reset on resume: only the first element was
// yielded (or the loop never ended).
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

BEZEL_TEST(codegen_iterator_case_arm_range_for_6951, first_arm_range) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method G(k: int): IEnumerable<int> "
            "   case k of "
            "     1 => for var c in 200..<207 do yield return c; end;, "
            "     _ => yield return 9; "
            "   end; end G; "
            " public static method Main() "
            "   for var v in G(1) do Console.WriteLine(v); end; "
            "   for var v in G(2) do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("200\n201\n202\n203\n204\n205\n206\n9\n"));
}

BEZEL_TEST(codegen_iterator_case_arm_range_for_6951, default_arm_with_step) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method G(k: int): IEnumerable<int> "
            "   case k of "
            "     1 => for var c in 200..<207 step 3 do yield return c; end;, "
            "     _ => for var d in 300..<307 step 3 do yield return d; end; "
            "   end; end G; "
            " public static method Main() "
            "   for var v in G(1) do Console.WriteLine(v); end; "
            "   for var v in G(0) do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("200\n203\n206\n300\n303\n306\n"));
}

BEZEL_TEST(codegen_iterator_case_arm_range_for_6951, runtime_step_and_hi_in_arm) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method G(k: int, st: long, hi: int): IEnumerable<int> "
            "   case k of "
            "     1 => for var c in 0..<hi step st do yield return c; end;, "
            "     _ => yield return -1; "
            "   end; end G; "
            " public static method Main() "
            "   for var v in G(1, 2L, 7) do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("0\n2\n4\n6\n"));
}

BEZEL_TEST(codegen_iterator_case_arm_range_for_6951, if_else_and_try_nesting) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method G(k: int): IEnumerable<int> "
            "   if k > 5 then "
            "     for var a in 0..<2 do yield return a; end; "
            "   else "
            "     case k of "
            "       1 => do "
            "              try "
            "                for var c in 10..<13 do yield return c; end; "
            "              finally Console.WriteLine(\"fin\"); end try; "
            "            end, "
            "       _ => for var d in 20..<22 do yield return d; end; "
            "     end; "
            "   end; end G; "
            " public static method Main() "
            "   for var v in G(9) do Console.WriteLine(v); end; "
            "   for var v in G(1) do Console.WriteLine(v); end; "
            "   for var v in G(0) do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("0\n1\n10\n11\n12\nfin\n20\n21\n"));
}

// Controls: `while` in an arm, and a plain method with a `for` in an arm.
BEZEL_TEST(codegen_iterator_case_arm_range_for_6951, controls) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method G(k: int): IEnumerable<int> "
            "   var i := 0; "
            "   case k of "
            "     1 => while i < 3 do yield return i; i := i + 1; end;, "
            "     _ => yield return 9; "
            "   end; end G; "
            " public static method Pl(k: int) "
            "   case k of "
            "     1 => for var c in 0..<3 do Console.WriteLine(c); end;, "
            "     _ => Console.WriteLine(9); "
            "   end; end Pl; "
            " public static method Main() "
            "   for var v in G(1) do Console.WriteLine(v); end; "
            "   Pl(1); "
            " end Main; end Program;"),
        std::string("0\n1\n2\n0\n1\n2\n"));
}
