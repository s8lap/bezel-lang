// Regression tests for issue #9059 — §28.3 closure bullet *Provenance*
// clause 6 (v0.359): an assignment REPLACES a local's provenance.
//
// THE DEFECT.  `MethodOwnership::setErasingSourceSet` answered "NON-EMPTY"
// for any local present in `callBoundedAlias_` — a map built flow-
// insensitively by the call-bounded scan — before consulting the flow facts
// (`seFlowDefs_`) that implement clause 6.  So in
//
//     var f: () -> int := q;      // q: plain function-typed parameter
//     f := lambda() => 9;         // capture-free overwrite
//     rg.Keep(f);                 // Keep(o: Object): a set-erasing position
//
// `f` stayed "carrying q" after the overwrite and the capture-free closure
// was rejected at `Object`.  Clause 6: "the assignments that reach the read"
// decide the set, and the overwrite dominates the read, so the parameter's
// value no longer reaches `f` and "a capture-free one is admitted".
//
// ROWS.  The two admitted rows are RED on the base tree (rejected there with
// the set-erasing diagnostic); they run 200 loops and pin exact stdout.  They
// use `runAot`, not `runAotChecked`, because converting a closure local to
// `Object` leaks the closure independently of this rule (issue #9061); the
// ASan run of these programs shows no use-after-free, only that leak.  The two REJECT rows are
// base-green boundary controls: the overwrite lives in only ONE arm of an
// `if` (the alias reaches the read through the other arm), and there is no
// overwrite at all (#8963 / #9044's shape).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kAtObject =
    "may not be passed where the parameter is declared Object";

std::string program(const std::string& methodBody, const std::string& call) {
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
           "  public static method Via(rg: Reg2, q: () -> int, c: bool) " +
           methodBody +
           "  end Via; "
           "  public static method Mk(reg: Reg2, r: Res) " +
           call +
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

std::string expected9() {
    std::string s;
    for (int i = 0; i < 200; ++i) s += "9\n";
    return s;
}

}  // namespace

// ADMIT — the issue's probe: a block-bodied lambda inside a method body
// aliases its own function-typed parameter, overwrites the alias with a
// capture-free lambda, and passes it at `Object`.  RED on base (rejected).
BEZEL_TEST(call_bounded_alias_overwrite_9059,
           lambda_param_alias_overwritten_capture_free_admitted_spec_28_3) {
    auto r = runAot(program(
        "  return; ",
        "    var h: (Reg2, () -> int) -> () := "
        "      lambda(rg: Reg2, q: () -> int) => do "
        "        var f: () -> int := q; "
        "        f := lambda() => 9; "
        "        rg.Keep(f); "
        "      end; "
        "    h(reg, lambda() => r.V); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected9());
}

// ADMIT — the same shape with the method's OWN parameter `q` as the source.
// RED on base (rejected).
BEZEL_TEST(call_bounded_alias_overwrite_9059,
           method_param_alias_overwritten_capture_free_admitted_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; "
        "    f := lambda() => 9; "
        "    rg.Keep(f); ",
        "    Via(reg, lambda() => r.V, true); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected9());
}

// REJECT (control, base-green) — the overwrite is in ONE arm of an `if`, so
// the parameter's value reaches the read through the other arm.
BEZEL_TEST(call_bounded_alias_overwrite_9059,
           overwrite_in_one_branch_still_rejected_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; "
        "    if c then f := lambda() => 9; end if; "
        "    rg.Keep(f); ",
        "    Via(reg, lambda() => r.V, true); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// REJECT (control, base-green) — no overwrite at all.
BEZEL_TEST(call_bounded_alias_overwrite_9059,
           alias_without_overwrite_still_rejected_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; "
        "    rg.Keep(f); ",
        "    Via(reg, lambda() => r.V, true); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// ADMIT — BOTH arms of an `if`/`else` overwrite the alias with a capture-free
// lambda, so the parameter's value reaches the read through no path (clause
// 6: "the assignments that reach the read").  RED on base (rejected).
BEZEL_TEST(call_bounded_alias_overwrite_9059,
           overwrite_in_both_branches_admitted_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; "
        "    if c then f := lambda() => 9; else f := lambda() => 9; end if; "
        "    rg.Keep(f); ",
        "    Via(reg, lambda() => r.V, true); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected9());
}
