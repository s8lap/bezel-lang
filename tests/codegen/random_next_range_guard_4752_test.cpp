// fix #4752 — stdlib §2.7 (v0.307): `Random.Next`'s two bounded overloads
// declare a fault condition that the lowering never raised.
//
// THE RULE, read off stdlib §2.7 before the guard was written:
//
//   public method Next(maxValue: int): int;
//       // 0..<maxValue; maxValue < 0: ArgumentOutOfRangeException
//   public method Next(minValue: int, maxValue: int): int;
//       // minValue > maxValue: ArgumentOutOfRangeException
//
// Both conditions are stated on the declaration itself, so they are the
// member's contract, not an implementation detail. `minValue = maxValue` is
// NOT a fault — §2.7's one-argument form is documented half-open
// (`0..<maxValue`), so the equal-bounds case is the documented empty range
// and must stay admissible; the guard is therefore strictly greater-than.
//
// WHAT WENT WRONG. Measured on 06796339 (docs v0.307) with the real driver:
// rule-corpus rows `l2_7_0003` (`r.Next(-5)`) and `l2_7_0004`
// (`r.Next(10, 1)`), each wrapping the call in
// `try … catch ArgumentOutOfRangeException`, both printed `no exception`.
// `LlvmBackend_Calls.cpp`'s instance-`Random` arm lowered straight to
// `bz_random_next_int` with the bad bounds, where `Math.DivRem` (#3580) and
// `/` (§6.1) both emit an `emitGuardThrow` before the operation.
//
// ORACLE NOTE. These are THROW rows, so the observable is routed to stdout
// rather than to an exit code: the `catch` arm prints a sentinel and the test
// compares the captured text. An exit-code-only oracle would be weak here —
// exit 1 is also what a program aborting under AddressSanitizer returns, and
// `Next`'s result is a random value that cannot be asserted directly.
//
// TEST MATRIX:
//   T1  `Next(maxValue)` with a negative bound raises (corpus l2_7_0003).
//   T2  `Next(minValue, maxValue)` with the bounds crossed raises
//       (corpus l2_7_0004).
//   T3/T4  ACCEPT-CONTROLS: a valid `Next(max)` and `Next(min, max)` still
//       run and stay inside their declared range. Without these the guards
//       could be inverted and T1/T2 would still be green.
//   T5  the documented EMPTY range `Next(n, n)` is not a fault — the guard
//       must be `>`, not `>=`.
//   T6  the guard is on the VALUE, not on a literal: the bad bound arrives
//       through a variable, so a constant-folding-only check would miss it.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

void expectPrints(const bezel::test::JitResult& r, std::string_view want) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want) + "\n");
}

// T1 — rule-corpus l2_7_0003 (stdlib §2.7: "maxValue < 0:
// ArgumentOutOfRangeException"). Pre-fix: printed "no exception".
BEZEL_TEST(random_next_range_guard_4752, stdlib_2_7_next_max_negative_raises) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r := new Random(1);
    try
      _ := r.Next(-5);
      Console.WriteLine("no exception");
    catch
      ArgumentOutOfRangeException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
)");
    expectPrints(r, "threw");
}

// T2 — rule-corpus l2_7_0004 (stdlib §2.7: "minValue > maxValue:
// ArgumentOutOfRangeException"). Pre-fix: printed "no exception".
BEZEL_TEST(random_next_range_guard_4752, stdlib_2_7_next_min_gt_max_raises) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r := new Random(1);
    try
      _ := r.Next(10, 1);
      Console.WriteLine("no exception");
    catch
      ArgumentOutOfRangeException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
)");
    expectPrints(r, "threw");
}

// T3 — ACCEPT-CONTROL: a well-formed `Next(maxValue)` still runs and stays in
// `0..<maxValue` (stdlib §2.7). Guards the inverted-predicate failure mode.
BEZEL_TEST(random_next_range_guard_4752, stdlib_2_7_next_max_valid_in_range) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r := new Random(1);
    var v := r.Next(10);
    Console.WriteLine(v >= 0 and v < 10);
  end Main;
end Program;
)");
    expectPrints(r, "True");
}

// T4 — ACCEPT-CONTROL for the two-argument overload.
BEZEL_TEST(random_next_range_guard_4752, stdlib_2_7_next_min_max_valid_in_range) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r := new Random(7);
    var v := r.Next(5, 9);
    Console.WriteLine(v >= 5 and v < 9);
  end Main;
end Program;
)");
    expectPrints(r, "True");
}

// T5 — `minValue = maxValue` is the documented EMPTY range, not a fault:
// §2.7's bounded form is half-open, so equal bounds name no values but are
// not an error. This row fails if the guard is written `>=`.
BEZEL_TEST(random_next_range_guard_4752, stdlib_2_7_next_equal_bounds_not_a_fault) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r := new Random(1);
    try
      _ := r.Next(4, 4);
      Console.WriteLine("no exception");
    catch
      ArgumentOutOfRangeException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
)");
    expectPrints(r, "no exception");
}

// T6 — the guard tests the runtime VALUE. The bad bound reaches the call
// through a local, so a check that only inspected a literal argument would
// not fire.
BEZEL_TEST(random_next_range_guard_4752, stdlib_2_7_guard_is_on_the_value_not_a_literal) {
    auto r = runAot(R"(
module Conf;

public class Program
  public static method Main()
    var r := new Random(1);
    var lo := 100;
    var hi := lo - 99;
    try
      _ := r.Next(lo, hi);
      Console.WriteLine("no exception");
    catch
      ArgumentOutOfRangeException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
)");
    expectPrints(r, "threw");
}

}  // namespace
