// Regression tests for #7913 (§8.7, v0.350): `e.Dispose()` on an enum value
// compiled (rc=0) although enums do not implement `IDisposable`.
//
// §8.7 (v0.350): "`Dispose()` is class-only. The compiler generates
// `IDisposable` on every non-static class. Static classes, records, enums,
// choices, primitives, and inline arrays do not implement `IDisposable`".
//
// ROOT CAUSE: typeCall (ExprTypecheck_Calls.cpp) rejected `Dispose` on a
// receiver whose declaration is a Choice (§35.8) and had no arm for an Enum.
// Controls: the choice twin stays rejected; a class `Dispose()` still runs.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectEnumDisposeRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Dispose` is not admitted on an enum value"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""8.7"));
}

}  // namespace

// The issue's repro (§8.7): a local enum value, `e.Dispose()`.
BEZEL_TEST(enum_dispose_not_admitted_7913, issue_repro_local_enum_rejected_8_7) {
    expectEnumDisposeRejected(
        "module M; "
        "public enum E Red, Green; end E; "
        "public class Program "
        "  public static method Main() "
        "    var e := E.Red; "
        "    e.Dispose(); "
        "  end Main; "
        "end Program;");
}

// Other receiver spellings of an enum value (§8.7): the case read directly,
// a parameter, a field.
BEZEL_TEST(enum_dispose_not_admitted_7913, case_param_and_field_receivers_rejected_8_7) {
    expectEnumDisposeRejected(
        "module M; "
        "public enum E Red, Green; end E; "
        "public class Program "
        "  public static method Main() "
        "    E.Green.Dispose(); "
        "  end Main; "
        "end Program;");
    expectEnumDisposeRejected(
        "module M; "
        "public enum E Red, Green; end E; "
        "public class Program "
        "  public static method F(e: E) "
        "    e.Dispose(); "
        "  end F; "
        "  public static method Main() F(E.Red); end Main; "
        "end Program;");
    expectEnumDisposeRejected(
        "module M; "
        "public enum E Red, Green; end E; "
        "public class Holder "
        "  public e: E := E.Red; "
        "  public constructor() end constructor; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    var h := new Holder(); "
        "    h.e.Dispose(); "
        "  end Main; "
        "end Program;");
}

// Control (§8.7 "class-only"): a class Dispose() is still admitted and runs.
BEZEL_TEST(enum_dispose_not_admitted_7913, class_dispose_control_still_runs_8_7) {
    auto r = runAotChecked(
        "module M; "
        "public enum E Red, Green; end E; "
        "public class D "
        "  public constructor() end constructor; "
        "end D; "
        "public class Program "
        "  public static method Main() "
        "    var d := new D(); "
        "    d.Dispose(); "
        "    var e := E.Red; "
        "    Console.WriteLine(\"ok\"); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}
