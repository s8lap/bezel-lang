// fix #4825 — §5.1 / §22.6 / §22.7, v0.307.
//
// `int → IComparable<int>` is a §5.1 reference conversion: a primitive boxes
// into a builtin generic interface it implements (§14.2 clause 5, stdlib §9.1).
// Sema admits it for a single-candidate constructor; with a second, same-arity
// sibling, the positional call was rejected as if no conversion existed.
//
// ROOT CAUSE. `resolveOverload`'s per-candidate unjudgeable-param guard
// (lines ~1029–1036) returned `false` for `IComparable<int>` because it has
// `genericArgs` (generic instantiation). This marked the IComparable<int>
// candidate inapplicable before `classifyArg` was reached — even though
// `classifyArg` calls `implicitlyConvertible`, which correctly classifies the
// boxing conversion as `Convert`. Removing that guard lets the conversion be
// ranked, and the IComparable<int> candidate wins (the bool ctor's int→bool
// is NotApplicable).
//
// Spec: §5.1 reference conversions, §14.2 clause 5, §22.6 stage 3 (v0.307).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Returns true when the program was compiled and ran successfully,
// printing exactly the expected output (no trailing whitespace check).
bool runAndCheckOutput(const std::string& src, const char* expected) {
    auto r = runAot(src);
    if (!r.ran) return false;
    if (r.exitCode != 0) return false;
    // r.stdoutText is the captured stdout
    return r.stdoutText == expected;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// T1 — the issue's exact repro: two same-arity ctors, positional int arg.
// Expected: selects `constructor(v: IComparable<int>)`, prints "IC".
// ════════════════════════════════════════════════════════════════════════════

BEZEL_TEST(ctor_overload_icomparable_arg_4825,
           positional_int_arg_selects_icomparable_ctor_two_candidates) {
    BEZEL_EXPECT_TRUE(runAndCheckOutput(
        "module M;\n"
        "public class C\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(v: IComparable<int>) Tag := \"IC\"; end constructor;\n"
        "  public constructor(s: bool) Tag := \"B\"; end constructor;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var a := new C(5);\n"
        "    Console.WriteLine(a.Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "IC\n"));
}

// ════════════════════════════════════════════════════════════════════════════
// T2 — single-candidate regression guard: only `constructor(v: IComparable<int>)`
// exists. `new C(5)` must still compile cleanly (the single-candidate path
// was already correct, per the issue's note).
// ════════════════════════════════════════════════════════════════════════════

BEZEL_TEST(ctor_overload_icomparable_arg_4825,
           single_candidate_icomparable_ctor_still_accepted) {
    BEZEL_EXPECT_TRUE(runAndCheckOutput(
        "module M;\n"
        "public class C\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(v: IComparable<int>) Tag := \"IC\"; end constructor;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var a := new C(5);\n"
        "    Console.WriteLine(a.Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "IC\n"));
}

// ════════════════════════════════════════════════════════════════════════════
// T3 — control: class with only `constructor(s: bool)`. `new C(true)` must
// compile and print "B" (regression guard that the bool ctor is still
// reachable).
// ════════════════════════════════════════════════════════════════════════════

BEZEL_TEST(ctor_overload_icomparable_arg_4825,
           bool_ctor_still_selectable_when_only_bool_candidate) {
    BEZEL_EXPECT_TRUE(runAndCheckOutput(
        "module M;\n"
        "public class C\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(s: bool) Tag := \"T\"; end constructor;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var a := new C(true);\n"
        "    Console.WriteLine(a.Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "T\n"));
}

// ════════════════════════════════════════════════════════════════════════════
// T4 — int arg does NOT match bool ctor (int→bool is NotApplicable).
// Verifies the overload correctly rejects the bool candidate and selects
// IComparable<int> — the complement of the T3 control.
// ════════════════════════════════════════════════════════════════════════════

BEZEL_TEST(ctor_overload_icomparable_arg_4825,
           named_arg_also_routes_to_icomparable_ctor) {
    // Named argument form: also exercises the same §22.6 ranking.
    BEZEL_EXPECT_TRUE(runAndCheckOutput(
        "module M;\n"
        "public class C\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(v: IComparable<int>) Tag := \"IC\"; end constructor;\n"
        "  public constructor(s: bool) Tag := \"B\"; end constructor;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var a := new C(v := 5);\n"
        "    Console.WriteLine(a.Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "IC\n"));
}
