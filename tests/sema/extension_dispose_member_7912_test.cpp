// Regression tests for #7912 (§26.3, v0.350): an extension block declaring a
// member named `Dispose` or `DisposeAsync` compiled.
//
// §26.3 (v0.350): "No member named `Dispose` or `DisposeAsync`: the names are
// the disposal surface (§8.7), which an extension cannot supply — its receiver
// is a borrow it may not dispose (§26.7) — so the declaration is a compile
// error, the posture §28.2 takes for a class or record."
//
// ROOT CAUSE: the §26.3 per-member restriction loop in ExprTypecheck.cpp
// (static / virtual-kind / visibility / indexer ...) had no name check.
// Controls: an extension method with another name still compiles and runs.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string program(const std::string& ext) {
    return "module M; "
           "public enum E Red, Green; end E; "
           "public class K public constructor() end constructor; end K; " +
           ext +
           "public class Program "
           "  public static method Main() "
           "    Console.WriteLine(\"hi\"); "
           "  end Main; "
           "end Program;";
}

void expectRejected(const std::string& ext, const char* name) {
    auto r = runAot(program(ext));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("may not declare a member named `") + name + "`").c_str()));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""26.3"));
}

}  // namespace

// The four issue repros (§26.3): enum, class and primitive heads, `Dispose`,
// and the enum head with `DisposeAsync`.
BEZEL_TEST(extension_dispose_member_7912, enum_head_dispose_rejected_26_3) {
    expectRejected("extension E public method Dispose() end Dispose; end extension; ",
                   "Dispose");
}
BEZEL_TEST(extension_dispose_member_7912, class_head_dispose_rejected_26_3) {
    expectRejected("extension K public method Dispose() end Dispose; end extension; ",
                   "Dispose");
}
BEZEL_TEST(extension_dispose_member_7912, primitive_head_dispose_rejected_26_3) {
    expectRejected("extension int public method Dispose() end Dispose; end extension; ",
                   "Dispose");
}
BEZEL_TEST(extension_dispose_member_7912, enum_head_disposeasync_rejected_26_3) {
    expectRejected(
        "extension E public method DisposeAsync() end DisposeAsync; end extension; ",
        "DisposeAsync");
}

// "No member named": a computed property is a member too (§26.3 admits them).
BEZEL_TEST(extension_dispose_member_7912, property_named_dispose_rejected_26_3) {
    expectRejected("extension int public property Dispose: int => 1; end extension; ",
                   "Dispose");
}

// Control: a neighbouring name is admitted and runs (§26.3 forbids exactly
// the two names).
BEZEL_TEST(extension_dispose_member_7912, other_names_control_runs_26_3) {
    auto r = runAotChecked(
        "module M; "
        "extension int "
        "  public method Disposed(): int => this + 1; "
        "  public method MyDispose(): int => this + 2; "
        "end extension; "
        "public class Program "
        "  public static method Main() "
        "    var n := 1; "
        "    Console.WriteLine(n.Disposed() + n.MyDispose()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}
