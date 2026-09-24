// fix #4852 (§22.6 "Defaulted-arity tie-break", v0.307): "a candidate
// applicable without filling any default is preferred over a candidate that
// fills one or more, provided the no-defaults candidate is better-or-equal at
// every supplied-argument position." `ctorForArgs`'s trailing defaulted-fill
// loop (src/codegen/LlvmBackend_impl.hpp) is the only one of its three arms
// that can select a candidate filling a §22.5 default (the ==1 and >1 arms
// above it only ever return a candidate whose OWN arity equals `args.size()`
// exactly). It consulted `primaryStrictlyApplicable()` only in the
// `searchOnce() == nullptr` fallback further down `ctorForArgs`, so a §22.10
// primary constructor that is exactly (no-default) applicable never got a
// chance to out-rank a defaulted explicit sibling the trailing loop had
// already matched.
//
// Regression found by the non-author review of the #4790 rework (c7ac00b7):
// removing the old `==1`-arity-arm null fallback (which happened to route
// back to the primary as a side effect) exposed this gap for the FIRST time
// on a class with a type-mismatched arity-1 sibling present (Repro 1 below);
// probing further showed the identical mis-selection ALSO on the baseline
// (pre-#4790) whenever no such sibling exists at all (Repro 2), because
// `ctorForArgs` never implemented this tie-break to begin with — the series
// just widened its reach. See #4838 (record primary vs. body ctor) for the
// broader "codegen re-derives ctor resolution instead of consuming sema's"
// context these two probes sit inside.
//
// Both repros are the review's own probe files verbatim
// (/var/tmp/bezel-scratch/r20/rv-l1c/p/r1_primary_vs_defaulted_with_str_sibling.bzl,
// r2_primary_vs_defaulted.bzl). Every row is a VALUE assertion on stdout — the
// pre-fix defect compiles and runs to exit 0 with the WRONG number printed
// (105 instead of 5), so `r.ran`/exit-code alone would prove nothing.
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// Repro 1 (r1_primary_vs_defaulted_with_str_sibling.bzl): primary `(x: int)`
// plus a type-mismatched arity-1 sibling `(s: string)` plus the defaulted
// `(x: int, k: int := 7)`. `new C(5)`: the `(s: string)` sibling is the
// UNIQUE arity-1 candidate but fails `ctorTypeMatches` (int vs string), so
// the ==1 arm falls through; `ctorArityCount(cls, 1)` is exactly 1, so the >1
// arm never runs either; the trailing loop then matches `(x: int, k := 7)`
// via `ctorApplicableWithDefaults` (k defaulted) and — pre-fix — returned it
// immediately, printing `105`. The primary is exact (fills no default) and
// strictly applicable, so §22.6 prefers it: expected `5`.
BEZEL_TEST(codegen_ctor_primary_defaulted_tiebreak_4852,
           primary_beats_defaulted_sibling_with_mismatched_sibling_spec_22_6) {
    auto r = runAot(
        "module M; "
        "public class C(x: int) "
        "  public constructor(s: string) this(s.Length + 1000); end constructor; "
        "  public constructor(x: int, k: int := 7) this(x + 100); end constructor; "
        "  public method Get(): int return x; end Get; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(5).Get().ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// Repro 2 (r2_primary_vs_defaulted.bzl): the identical primary-vs-defaulted
// shape with NO other sibling at all. Here `ctorArityCount(cls, 1)` is 0 (the
// only explicit ctor has arity 2), so neither the ==1 nor the >1 arm ever
// runs — ONLY the trailing loop resolves this call, on both the pre-#4790
// baseline and the #4790-rework series alike, so this mis-selection is
// STRICTLY OLDER than #4790 and not introduced by it. It is fixed here as a
// direct consequence of the same tie-break guard (`ctorForArgs` never
// distinguished the two shapes), not as separate scope: expected `5`, not the
// pre-fix `105`.
BEZEL_TEST(codegen_ctor_primary_defaulted_tiebreak_4852,
           primary_beats_defaulted_sibling_no_other_sibling_spec_22_6) {
    auto r = runAot(
        "module M; "
        "public class C(x: int) "
        "  public constructor(x: int, k: int := 7) this(x + 100); end constructor; "
        "  public method Get(): int return x; end Get; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(5).Get().ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// Loop-amplified (50 iterations, Repro 1's shape): the ctor selection made at
// this `new` call site is a compile-time choice exercised identically on
// every iteration, so a wrong selection contributes a wrong term to the sum
// on all 50 draws rather than a possibly-coincidentally-right one-shot value.
BEZEL_TEST(codegen_ctor_primary_defaulted_tiebreak_4852,
           loop_50_iterations_spec_22_6) {
    auto r = runAot(
        "module M; "
        "public class C(x: int) "
        "  public constructor(s: string) this(s.Length + 1000); end constructor; "
        "  public constructor(x: int, k: int := 7) this(x + 100); end constructor; "
        "  public method Get(): int return x; end Get; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var i in 0..<50 do "
        "      sum := sum + new C(i).Get(); "
        "    end; "
        "    Console.WriteLine(sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Pre-fix every draw resolves to `(x, k := 7)`'s `this(x + 100)` chain,
    // summing to sum_{i=0}^{49} (i + 100) = 1225 + 5000 = 6225. Post-fix the
    // primary wins every draw: sum_{i=0}^{49} i = 1225.
    BEZEL_EXPECT_EQ(r.stdoutText, "1225\n");
}

// Control (unaffected by the #4852 guard, since `C` here has NO §22.10
// primary at all): `r3_this_chain_str_sibling.bzl` from the same review —
// pins that the guard's `cls->hasPositionalFields` gate correctly leaves the
// #4790 rework's OWN improvement (a `this(...)` self-chain call now reaching
// a defaulted sibling past an unrelated type-mismatched arity-1 sibling)
// untouched. Pre-#4790 this printed two empty lines (`this(5)` and
// `this(3)`-by-name found no applicable target); post-#4790-rework, both
// resolve to `(n: int, k := 7)`.
BEZEL_TEST(codegen_ctor_primary_defaulted_tiebreak_4852,
           control_no_primary_defaulted_sibling_unaffected_spec_22_6) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public readonly property Tag: string; "
        "  public constructor(s: string) Tag := $\"S{s}\"; end constructor; "
        "  public constructor(n: int, k: int := 7) Tag := $\"N{n},{k}\"; end constructor; "
        "  public constructor(a: bool, b: bool) this(5); end constructor; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(true, false).Tag); "
        "    Console.WriteLine(new C(3).Tag); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "N5,7\nN3,7\n");
}
