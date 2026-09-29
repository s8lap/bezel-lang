// regression pin for #4995 (fixed by #4825 0526620e) — §22.6, v0.307.
//
// `int → IComparable<int>` and `int → IEquatable<int>` are §5.1 reference
// conversions: a primitive boxes into a builtin generic interface it
// implements (§14.2 clause 5, stdlib §9.1). At a `new C(int)` call site with a
// SAME-ARITY sibling that has a defaulted parameter, the §22.6 stage 6
// defaulted-arity tie-break picks the sibling that fills no default — which
// would be the interface candidate, NOT the primitive-widening sibling. The
// pre-#4825 code rejected both interface candidates as inapplicable (the
// `judgeable()` per-candidate guard) and let the primitive sibling win on its
// own; the post-#4825 code classifies the boxing conversion uniformly and the
// interface candidate wins.
//
// These rows are the issue #4995 q9 and q10 repros from
// /var/tmp/bezel-scratch/r20/rv-l1c/p/. They are regression pins, not new
// behaviour: the underlying fix landed in #4825 (commit 0526620e) and is
// already on the chain. The rows exist so any future revert or guard-restoration
// in resolveOverload fails the suite, not so the bug returns.
//
// Spec: §5.1 reference conversions, §14.2 clause 5, §22.6 stage 6 (v0.307).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Returns true when the program compiled, ran successfully, and printed
// exactly the expected output (no trailing whitespace check).
bool runAndCheckOutput(const std::string& src, const char* expected) {
    auto r = runAot(src);
    if (!r.ran) return false;
    if (r.exitCode != 0) return false;
    return r.stdoutText == expected;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// q9 — issue #4995's first repro
//   (`q9_box_vs_doubledefault.bzl` in the r20 scratch dir)
// Two same-arity ctors:
//   - constructor(v: IComparable<int>)            — primitive boxes into the
//     builtin generic interface (§5.1, §14.2 clause 5). Conversion rank:
//     Convert.
//   - constructor(d: double, k: int := 0)         — int→double is a §6.6
//     primitive widening. Conversion rank: Convert. The defaulted `k` is
//     unfilled at the call site, so §22.6 stage 6 prefers the interface ctor.
// Expected: prints "IC".
// ════════════════════════════════════════════════════════════════════════════

BEZEL_TEST(ctor_overload_boxed_iface_arg_4995,
           q9_icomparable_vs_double_defaulted_picks_icomparable) {
    BEZEL_EXPECT_TRUE(runAndCheckOutput(
        "module M;\n"
        "public class C\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(v: IComparable<int>) Tag := \"IC\"; end constructor;\n"
        "  public constructor(d: double, k: int := 0) Tag := \"D\"; end constructor;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new C(5).Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "IC\n"));
}

// ════════════════════════════════════════════════════════════════════════════
// q10 — issue #4995's second repro
//   (`q10_box_vs_longdefault.bzl` in the r20 scratch dir)
// Two same-arity ctors:
//   - constructor(v: IEquatable<int>)             — primitive boxes into the
//     builtin generic interface (§5.1, §14.2 clause 5). Conversion rank:
//     Convert.
//   - constructor(l: long, k: int := 0)           — int→long is a §6.6
//     primitive widening. Conversion rank: Convert. The defaulted `k` is
//     unfilled at the call site, so §22.6 stage 6 prefers the interface ctor.
// Expected: prints "IE".
// ════════════════════════════════════════════════════════════════════════════

BEZEL_TEST(ctor_overload_boxed_iface_arg_4995,
           q10_iequatable_vs_long_defaulted_picks_iequatable) {
    BEZEL_EXPECT_TRUE(runAndCheckOutput(
        "module M;\n"
        "public class C\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(v: IEquatable<int>) Tag := \"IE\"; end constructor;\n"
        "  public constructor(l: long, k: int := 0) Tag := \"L\"; end constructor;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new C(5).Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "IE\n"));
}
