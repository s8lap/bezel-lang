// fix #754 / §27.5: capturing the for-loop variable in a closure declared
// inside a NESTED if-block must not break for-loop control flow.
//
// §27.5 gives each iteration's loop variable a fresh binding for capture. The
// fresh-cell boxing used to be emitted lazily at the capturing lambda site;
// when that site sat inside a conditionally-executed `if`, iterations that
// skipped the branch never refreshed the cell, so later reads of the loop
// variable (now routed through the cell) observed a stale value and the loop
// miscompiled. The fix pre-boxes the captured loop variable at body entry so
// the cell is refreshed on EVERY iteration (lowerForBody / forVarCapturedInBody).
//
// runJit exercises the codegen path (closures are a codegen concern, not an
// ownership one), so these tests use the JIT harness.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §27.5: the canonical #754 repro. A closure captures `i` inside `if i = 5`;
// the `if i = 6` branch on a later iteration must still run. 1 + 20 = 21.
// Before the fix this returned 1 (the i=6 branch was silently skipped).
BEZEL_TEST(loop_var_capture_nested_if_754, range_for_nested_if_capture) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var hits := 0; "
        "    for var i in 5 .. 7 do "
        "      if i = 5 then "
        "        var f := () => i; "       // captures the loop variable
        "        hits := hits + 1; "
        "      end; "
        "      if i = 6 then "             // must still run on the i=6 iteration
        "        hits := hits + 20; "
        "      end; "
        "    end; "
        "    return hits; "                // 21
        "  end Main; "
        "end C;"), 21);
}

// §27.5: the captured closure observes the CORRECT iteration's value when the
// capture is in a nested if. i runs 1..5; at i=5 the closure captures 5.
BEZEL_TEST(loop_var_capture_nested_if_754, capture_value_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var result := 0; "
        "    for var i in 1 .. 5 do "
        "      if i = 5 then "
        "        var f := () => i; "
        "        result := f(); "          // 5
        "      end; "
        "    end; "
        "    return result; "
        "  end Main; "
        "end C;"), 5);
}

// §27.5: two captures from two distinct iterations (each inside its own nested
// if) see distinct, correct values. 5 + 6 = 11.
BEZEL_TEST(loop_var_capture_nested_if_754, two_captures_distinct) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var i in 1 .. 6 do "
        "      if i = 5 then var f := () => i; sum := sum + f(); end; "
        "      if i = 6 then var g := () => i; sum := sum + g(); end; "
        "    end; "
        "    return sum; "                 // 11
        "  end Main; "
        "end C;"), 11);
}

// §27.5: the same shape over an inline-array for-loop (not just a Range). The
// array-for path also routes through lowerForBody, so the nested-if capture
// must be correct here too. 1 + 20 = 21.
BEZEL_TEST(loop_var_capture_nested_if_754, array_for_nested_if_capture) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var hits := 0; "
        "    var arr: [3]int := [5, 6, 7]; "
        "    for var i in arr do "
        "      if i = 5 then "
        "        var f := () => i; "
        "        hits := hits + 1; "
        "      end; "
        "      if i = 6 then "
        "        hits := hits + 20; "
        "      end; "
        "    end; "
        "    return hits; "                // 21
        "  end Main; "
        "end C;"), 21);
}

// §27.5 control: a NON-capturing closure in the same nested-if shape already
// worked (the trigger requires capturing the loop variable). Pins that the fix
// does not regress the non-capturing case. 1 + 20 = 21.
BEZEL_TEST(loop_var_capture_nested_if_754, non_capturing_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var hits := 0; "
        "    for var i in 5 .. 7 do "
        "      if i = 5 then "
        "        var f := () => 9; "       // does NOT capture i
        "        hits := hits + 1; "
        "      end; "
        "      if i = 6 then "
        "        hits := hits + 20; "
        "      end; "
        "    end; "
        "    return hits; "                // 21
        "  end Main; "
        "end C;"), 21);
}
