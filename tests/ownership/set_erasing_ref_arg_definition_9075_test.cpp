// Regression tests for issue #9075 — §28.3 closure bullet *Provenance* clause
// 6 (v0.359): a local contributes, at each read, the union of the sets of the
// forms the flow analysis traces it to "over every assignment that reaches the
// read".
//
// THE DEFECT.  A `ref` argument the callee writes was a definition the set-
// erasing flow trace marked UNTRACED (admitted), so a call-bounded parameter's
// value moved into a local by a `ref` swap (`Swap(ref f, ref g)`) reached
// `Object` unseen and the stored closure was later called over a freed
// capture (heap-use-after-free).  A `ref` argument's definition now joins the
// current value of every `ref` argument of the same call, itself included.
//
// ROWS.  Each reject row is one of the issue's probes (s1, s2, s3, s7,
// ref_swap, ref_swap_chain) and is RED on the base (admitted).  The admit
// twin swaps two non-call-bounded closures and is a base-green control.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kAtObject =
    "may not be passed where the parameter is declared Object";

std::string program(const std::string& methodBody) {
    return "module M; "
           "import System; "
           "public class Res "
           "  public V: int; "
           "  public constructor() V := 42; end constructor; "
           "end Res; "
           "public class Reg2 "
           "  private _o: Object? := null; "
           "  public constructor() end constructor; "
           "  public method Keep(o: Object) _o := o; end Keep; "
           "  public method RunInt(): int "
           "    if _o is () -> int then return (_o as () -> int)!(); end if; "
           "    return 7; "
           "  end RunInt; "
           "end Reg2; "
           "public class Program "
           "  public static method Swap(ref a: () -> int, ref b: () -> int) "
           "    var t: () -> int := a; a := b; b := t; "
           "  end Swap; "
           "  public static method Via(rg: Reg2, q: () -> int, c: bool) " +
           methodBody +
           "  end Via; "
           "  public static method Mk(reg: Reg2, r: Res) "
           "    Via(reg, lambda() => r.V, true); "
           "  end Mk; "
           "  public static method Main(): int "
           "    using var reg := new Reg2(); "
           "    for var i in 0 ..< 200 do "
           "      do "
           "        var r := new Res(); "
           "        Mk(reg, r); "
           "      end; "
           "      Console.WriteLine(reg.RunInt()); "
           "    end for; "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// REJECT (RED on base) — the issue's probe `s1_swap_into_f`: q's closure is moved by
// the `ref` swap into the local that reaches the `Object` position.
BEZEL_TEST(set_erasing_ref_arg_definition_9075, swap_into_overwritten_alias_f_rejected_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; var g: () -> int := f; f := lambda() => 9; Swap(ref f, ref g); rg.Keep(f); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// REJECT (RED on base) — the issue's probe `s2_swap_h_g`: q's closure is moved by
// the `ref` swap into the local that reaches the `Object` position.
BEZEL_TEST(set_erasing_ref_arg_definition_9075, swap_h_g_chain_rejected_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; var g: () -> int := f; var h: () -> int := g; f := lambda() => 9; g := f; Swap(ref h, ref g); rg.Keep(g); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// REJECT (RED on base) — the issue's probe `s3_swap_then_chain`: q's closure is moved by
// the `ref` swap into the local that reaches the `Object` position.
BEZEL_TEST(set_erasing_ref_arg_definition_9075, swap_then_chain_rejected_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; var g: () -> int := lambda() => 9; var h: () -> int := g; Swap(ref f, ref g); h := g; rg.Keep(h); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// REJECT (RED on base) — the issue's probe `s7_swap_alias_inner`: q's closure is moved by
// the `ref` swap into the local that reaches the `Object` position.
BEZEL_TEST(set_erasing_ref_arg_definition_9075, swap_alias_inner_rejected_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; var z: () -> int := lambda() => 9; var g: () -> int := f; Swap(ref f, ref z); g := z; rg.Keep(g); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// REJECT (RED on base) — the issue's probe `ref_swap`: q's closure is moved by
// the `ref` swap into the local that reaches the `Object` position.
BEZEL_TEST(set_erasing_ref_arg_definition_9075, ref_swap_rejected_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; var g: () -> int := lambda() => 9; Swap(ref f, ref g); rg.Keep(g); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// REJECT (RED on base) — the issue's probe `ref_swap_chain`: q's closure is moved by
// the `ref` swap into the local that reaches the `Object` position.
BEZEL_TEST(set_erasing_ref_arg_definition_9075, ref_swap_chain_rejected_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; var g: () -> int := lambda() => 9; var h: () -> int := f; Swap(ref f, ref g); h := g; rg.Keep(h); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// ADMIT (named base-green control) — both swapped closures are capture-free
// (neither is call-bounded), so no non-empty set reaches the `Object`.
BEZEL_TEST(set_erasing_ref_arg_definition_9075,
           swap_of_two_capture_free_closures_admitted_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := lambda() => 9; "
        "    var g: () -> int := lambda() => 8; "
        "    Swap(ref f, ref g); "
        "    rg.Keep(f); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string want;
    for (int i = 0; i < 200; ++i) want += "8\n";
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}
