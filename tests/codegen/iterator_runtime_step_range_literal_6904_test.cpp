// Spec-derived regression test for issue #6904.
//
// Spec §16.4 (v0.340): a range expression's `step` is evaluated once, at the
// range expression, and may be any `long` expression; the loop ascends for a
// positive step and descends for a negative one. §16.3: `lo..hi` is inclusive
// of `hi`, `lo..<hi` exclusive. Spec §29.3 (v0.340): an iterator method is a
// state machine; a value the loop needs after a `yield` must survive the
// suspend.
//
// THE DEFECT. In an iterator method, `for var x in lo..hi step st do yield
// return x; end;` with a non-constant `st` computed the step as an SSA value in
// the block that first reaches the loop; the resume path enters `for.cond` /
// `for.inc` without passing through it, so LLVM verification failed with
// "Instruction does not dominate all uses!". The non-iterator form and the
// constant-step iterator form compiled. The step now lives in a frame-backed
// slot in the state machine.
//
// Oracle: `runAotChecked` (sanitizers armed, exit 0 required) compared on exact
// stdout.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
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

// §16.4 / §29.3: the issue's repro — a parameter step, ascending.
BEZEL_TEST(codegen_iterator_runtime_step_range_literal_6904,
           parameter_step_ascending_16_4) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method G(lo: int, hi: int, st: long): IEnumerable<int> "
            "   for var x in lo..hi step st do yield return x; end; end G; "
            " public static method Main() "
            "   for var v in G(1,9,3L) do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("1\n4\n7\n"));
}

// §16.4: a negative parameter step descends; `..<` excludes the bound.
BEZEL_TEST(codegen_iterator_runtime_step_range_literal_6904,
           parameter_step_descending_16_4) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method G(lo: int, hi: int, st: long): IEnumerable<int> "
            "   for var x in lo..hi step st do yield return x; end; end G; "
            " public static method H(lo: int, hi: int, st: long): IEnumerable<int> "
            "   for var x in lo..<hi step st do yield return x; end; end H; "
            " public static method Main() "
            "   for var v in G(9,1,-4L) do Console.WriteLine(v); end; "
            "   for var v in H(9,1,-4L) do Console.WriteLine(v); end; "
            "   for var v in H(8,0,-4L) do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("9\n5\n1\n9\n5\n8\n4\n"));
}

// §16.4: the step read from a field of the receiver.
BEZEL_TEST(codegen_iterator_runtime_step_range_literal_6904,
           field_step_16_4) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Box "
            "  private _st: long; "
            "  public constructor(s: long) this._st := s; end constructor; "
            "  public method G(lo: int, hi: int): IEnumerable<int> "
            "    for var x in lo..hi step this._st do yield return x; end; "
            "  end G; end Box; "
            "public class Program "
            " public static method Main() "
            "   var b := new Box(3L); "
            "   for var v in b.G(1,9) do Console.WriteLine(v); end; "
            "   var c := new Box(-3L); "
            "   for var v in c.G(9,1) do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("1\n4\n7\n9\n6\n3\n"));
}

// §16.4: the step is evaluated ONCE (at the range expression): a local step
// reassigned inside the loop body, across a yield, does not change the loop.
BEZEL_TEST(codegen_iterator_runtime_step_range_literal_6904,
           local_step_evaluated_once_across_yield_16_4) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method L(): IEnumerable<int> "
            "   var st: long := 3L; "
            "   for var x in 1..<10 step st do st := 100L; yield return x; end; "
            " end L; "
            " public static method Main() "
            "   for var v in L() do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("1\n4\n7\n"));
}

// Controls: the non-iterator runtime step and the iterator literal step.
BEZEL_TEST(codegen_iterator_runtime_step_range_literal_6904,
           controls_non_iterator_and_literal_step_16_4) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; public class Program "
            " public static method C(lo: int, hi: int, st: long): int "
            "   var c: int := 0; "
            "   for var x in lo..hi step st do c := c + 1; end; return c; "
            " end C; "
            " public static method G(lo: int, hi: int): IEnumerable<int> "
            "   for var x in lo..hi step 3 do yield return x; end; end G; "
            " public static method Main() "
            "   Console.WriteLine(C(1,9,3L)); Console.WriteLine(C(9,1,-3L)); "
            "   for var v in G(1,9) do Console.WriteLine(v); end; "
            " end Main; end Program;"),
        std::string("3\n3\n1\n4\n7\n"));
}
