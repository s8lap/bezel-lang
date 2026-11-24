// Regression tests for issue #9072 — §28.3 closure bullet *Provenance*
// clause 6 (v0.359): "over every assignment that reaches the read".
//
// THE DEFECT.  After #9059, a local aliased from a call-bounded parameter is
// judged by its reaching definitions, but a reaching definition whose VALUE is
// ANOTHER alias local (`g := f`) still asked the flow-insensitive
// `callBoundedAlias_` map, so `g` stayed "carrying q" after `f` was
// overwritten with a capture-free lambda and was rejected at `Object`.
//
// ROWS.  The two admitted rows are RED on the base (rejected); they run 200
// loops and pin stdout via `runAot` (`runAotChecked` leaks, #9061).  The
// reject control reads the chained alias BEFORE the overwrite.

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

std::string expected9() {
    std::string s;
    for (int i = 0; i < 200; ++i) s += "9\n";
    return s;
}

}  // namespace

// ADMIT (RED on base) — the issue's probe a08.
BEZEL_TEST(call_bounded_alias_overwrite_9072,
           chained_alias_after_overwrite_admitted_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; "
        "    var g: () -> int := f; "
        "    f := lambda() => 9; "
        "    g := f; "
        "    rg.Keep(g); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected9());
}

// ADMIT (RED on base) — the issue's probe x16.
BEZEL_TEST(call_bounded_alias_overwrite_9072,
           as_object_after_both_arms_overwrite_admitted_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; "
        "    if c then f := lambda() => 9; else f := lambda() => 9; end if; "
        "    var o: Object := f; "
        "    rg.Keep(o); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected9());
}

// REJECT (control) — the chained alias is taken BEFORE the overwrite, so q
// reaches the read of `g`.
BEZEL_TEST(call_bounded_alias_overwrite_9072,
           chained_alias_before_overwrite_still_rejected_spec_28_3) {
    auto r = runAot(program(
        "    var f: () -> int := q; "
        "    var g: () -> int := f; "
        "    f := lambda() => 9; "
        "    rg.Keep(g); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}
