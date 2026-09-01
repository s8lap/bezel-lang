// Fix #3602 — `for var k in <dict>.Keys` with a `yield` in the loop body.
// Spec: §29.2, §29.3, and stdlib §4.8 (v0.286).
// The iterator must resume after the yield and preserve the dictionary snapshot.
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// #3602 / §29.2, §29.3: the Keys view must survive a yield/resume boundary.
BEZEL_TEST(dict_keys_values_yield_3602, keys_suspends_in_body) {
    auto output = runAotOut(
        "module M; class Src "
        "  private owned _d: Dictionary<int, int>; "
        "  public constructor() _d := new Dictionary<int, int>(); _d[3] := 30; end constructor; "
        "  public method Walk(): IEnumerable<int> "
        "    for var k in _d.Keys do yield return k; end for; "
        "  end Walk; "
        "end Src; "
        "class Main public static method Main(): int "
        "  using var s := new Src(); var t := 0; "
        "  for var v in s.Walk() do t := t + v; end for; "
        "  Console.WriteLine(t); return 0; "
        "end Main; end Main;");
    BEZEL_EXPECT_EQ(output, "3");
}

// The sibling Values view used the same snapshot state-machine shape.
BEZEL_TEST(dict_keys_values_yield_3602, values_suspends_in_body) {
    auto output = runAotOut(
        "module M; class Src "
        "  private owned _d: Dictionary<int, int>; "
        "  public constructor() _d := new Dictionary<int, int>(); _d[3] := 30; end constructor; "
        "  public method Walk(): IEnumerable<int> "
        "    for var v in _d.Values do yield return v; end for; "
        "  end Walk; "
        "end Src; "
        "class Main public static method Main(): int "
        "  using var s := new Src(); var t := 0; "
        "  for var v in s.Walk() do t := t + v; end for; "
        "  Console.WriteLine(t); return 0; "
        "end Main; end Main;");
    BEZEL_EXPECT_EQ(output, "30");
}
