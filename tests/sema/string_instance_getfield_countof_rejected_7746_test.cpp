// Regression for #7746 -- the instance spellings `s.GetField(delim, n)` and
// `s.CountOf(sub)` were admitted by name although neither is a member of
// stdlib §9.6 `String`.
//
// per stdlib §9.6 + §1 "Normative scope" (v0.350): §9.6 declares string's whole
// public surface and implementations may add only "internal helper methods and
// private members"; the #7131 ruling says the calls must be rejected (a phantom
// instance member would also block a user `extension string` of that name,
// spec §26.5 rule 1). Sibling of #7706 (instance `Repeat`).
//
// ORACLE: the specific diagnostic at a real location on the rejection rows;
// exact stdout on the controls (a user extension named like the withdrawn
// member now binds, and a declared instance member still works).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& ext, const std::string& body) {
    return "module M;\n" + ext +
           "class C\n"
           "  public static method Main(): int\n"
           "    var s: string := \"a,b,c\";\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void expectRejected(const std::string& body, const std::string& needle) {
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

void expectStdout(const std::string& ext, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char kGF[] = "string member `.GetField` is not yet supported in codegen";
const char kCO[] = "string member `.CountOf` is not yet supported in codegen";

}  // namespace

BEZEL_TEST(sema_string_instance_getfield_countof_7746, getfield_rejected) {
    // per stdlib §9.6 + §1 (v0.350)
    expectRejected("Console.WriteLine(s.GetField(',', 1));", kGF);
}

BEZEL_TEST(sema_string_instance_getfield_countof_7746, countof_rejected) {
    // per stdlib §9.6 + §1 (v0.350)
    expectRejected("Console.WriteLine(s.CountOf(\",\"));", kCO);
}

BEZEL_TEST(sema_string_instance_getfield_countof_7746, literal_receiver_rejected) {
    // per stdlib §9.6 + §1 (v0.350)
    expectRejected("var t: string := \"a,b\".GetField(',', 0);\n"
                   "    Console.WriteLine(t);", kGF);
}

BEZEL_TEST(sema_string_instance_getfield_countof_7746, wrong_arity_is_the_non_member_error) {
    // per stdlib §9.6 (v0.350): the old "lowers only at N arguments" arity
    // row is gone; the call is rejected as a non-member.
    expectRejected("Console.WriteLine(s.GetField(',', 1, 2));", kGF);
}

BEZEL_TEST(sema_string_instance_getfield_countof_7746, null_conditional_rejected) {
    // per stdlib §9.6 (v0.350)
    expectRejected("var t: string? := s;\n"
                   "    var r := t?.CountOf(\",\");", "`?.CountOf`");
}

// Controls.
BEZEL_TEST(sema_string_instance_getfield_countof_7746, control_user_extension_countof_binds) {
    // per spec §26.5 rule 2 (v0.350): string declares no instance CountOf.
    expectStdout("extension string\n"
                 "  public method CountOf(sub: string): int => 42;\n"
                 "  public method GetField(d: char, n: int): string => \"EXT\";\n"
                 "end;\n",
                 "Console.WriteLine(s.CountOf(\",\"));\n"
                 "    Console.WriteLine(s.GetField(',', 1));",
                 "42\nEXT\n");
}

BEZEL_TEST(sema_string_instance_getfield_countof_7746, control_declared_members_still_work) {
    // per stdlib §9.6 (v0.350): IndexOf / Contains are declared.
    expectStdout("", "Console.WriteLine(s.IndexOf(\",\"));\n"
                     "    Console.WriteLine(s.Contains(\"b\"));",
                 "1\nTrue\n");
}
