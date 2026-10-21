// Spec-derived regression test for issue #6807.
//
// stdlib §3.1 (v0.340) declares exactly one overload,
// `Insert(index: int, value: string): StringBuilder`. A non-string value has no
// overload to bind, so it is a located sema error. Before the fix the call was
// accepted and codegen died with "IR verification failed" (a `bz_sb_insert`
// parameter type mismatch). The `string` rows are the controls that must stay
// admitted.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {
bool hasLocatedInsertError(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("no matching overload of `StringBuilder.Insert`") !=
                std::string::npos &&
            d.message.find("stdlib §3.1") != std::string::npos)
            return true;
    return false;
}

std::string prog(const std::string& stmt) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var ts := TimeSpan.FromSeconds(90.5);\n"
           "    var a := new StringBuilder(); a.Append(\"x\");\n"
           "    " + stmt + "\n"
           "    Console.WriteLine(a.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectRejected(const std::string& stmt) {
    auto r = runAot(prog(stmt));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasLocatedInsertError(r));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    // The Insert statement is on line 6 of the program text, column 5+.
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics()[0].message.find(":6:") !=
                      std::string::npos);
}
}  // namespace

// stdlib §3.1: issue repro (TimeSpan value).
BEZEL_TEST(stringbuilder_insert_non_string_6807, timespan_rejected) {
    expectRejected("a.Insert(0, ts);");
}
BEZEL_TEST(stringbuilder_insert_non_string_6807, int_rejected) {
    expectRejected("a.Insert(0, 5);");
}
BEZEL_TEST(stringbuilder_insert_non_string_6807, char_rejected) {
    expectRejected("a.Insert(0, 'c');");
}
BEZEL_TEST(stringbuilder_insert_non_string_6807, object_rejected) {
    expectRejected("a.Insert(0, new Object());");
}

// Controls: the declared overload still compiles and runs.
BEZEL_TEST(stringbuilder_insert_non_string_6807, string_control_admitted) {
    auto r = runAot(prog("a.Insert(0, \"ab\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("abx\n"));
}
BEZEL_TEST(stringbuilder_insert_non_string_6807, to_string_control_admitted) {
    auto r = runAot(prog("a.Insert(0, ts.ToString());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("00:01:30.5000000x\n"));
}
