// #8096 -- stdlib §2.4 (v0.355) declares
//   public static method Get(name: string): string?;   // null when absent
// so the call's result is `string?` (spec §7.1: `string` and `string?` are
// distinct types; §7.3/§7.4: an un-narrowed nullable does not convert to its
// non-null type). The call result was untyped, so both
//   var x: string := Environment.Get("A");
//   Console.WriteLine(Environment.Get("A"));   // stdlib §2.1: (), (string), (Object)
// compiled. Reject rows pin the §7 diagnostic; accept rows pin exact stdout.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

std::string prog(const std::string& body) {
    return "module M;\n"
           "import System;\n"
           "class C\n"
           "  public static method TakesNullable(s: string?): string\n"
           "    return s ?? \"<null>\";\n"
           "  end TakesNullable;\n"
           "  public static method Main(): int\n" + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectRejected(const std::string& body) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "nullable but the target type is non-nullable (§7)") ||
                      hasError(r, "§7"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

BEZEL_TEST(environment_get_nullable_result_8096, binding_to_a_string_slot_is_refused) {
    expectRejected("    var x: string := Environment.Get(\"BZ8096_A\");");
}

BEZEL_TEST(environment_get_nullable_result_8096, console_writeline_argument_is_refused) {
    expectRejected("    Console.WriteLine(Environment.Get(\"BZ8096_A\"));");
}

BEZEL_TEST(environment_get_nullable_result_8096, inferred_local_passed_on_is_refused) {
    expectRejected("    var s := Environment.Get(\"BZ8096_A\");\n"
                   "    Console.WriteLine(s);");
}

BEZEL_TEST(environment_get_nullable_result_8096, coalesce_is_accepted) {
    expectStdout("    Console.WriteLine(Environment.Get(\"BZ8096_ABSENT\") ?? \"d\");",
                 "d\n");
}

BEZEL_TEST(environment_get_nullable_result_8096, null_check_narrows) {
    expectStdout("    Environment.Set(\"BZ8096_B\", \"v\");\n"
                 "    var s := Environment.Get(\"BZ8096_B\");\n"
                 "    if s <> null then Console.WriteLine(s); end;\n"
                 "    var t := Environment.Get(\"BZ8096_ABSENT\");\n"
                 "    if t = null then Console.WriteLine(\"absent\"); end;",
                 "v\nabsent\n");
}

BEZEL_TEST(environment_get_nullable_result_8096, nullable_parameter_accepts_it) {
    expectStdout("    Environment.Set(\"BZ8096_C\", \"w\");\n"
                 "    Console.WriteLine(TakesNullable(Environment.Get(\"BZ8096_C\")));\n"
                 "    Console.WriteLine(TakesNullable(Environment.Get(\"BZ8096_ABSENT\")));",
                 "w\n<null>\n");
}
