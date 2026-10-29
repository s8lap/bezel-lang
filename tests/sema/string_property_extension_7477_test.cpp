// Spec-derived regression test for issue #7477: a same-named extension over
// a stdlib §9.6 PROPERTY of `string`, reached through the CALL spelling.
//
// stdlib §9.6 (v0.350): `public pure readonly property Length: int;` (and
// `IsEmpty: bool`, `IsAscii: bool`) on `String`.
//
// spec §26.5 rule 1 (v0.350): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` — of any arity, applicability,
// or accessibility — instance resolution is committed and extension blocks
// are **never consulted** for that name on that receiver. If the committed
// instance lookup then fails (no applicable overload, §22.5/§22.6, or the
// member is inaccessible, §13.1), the call is a compile error citing the
// instance member(s); it does not fall back to an extension (the diagnostic
// should note a same-named extension where one exists)."
//
// THE DEFECT, MEASURED on r30-c1 29864453: with
// `extension string public method Length(): int => 42; end;` in scope,
// `Console.WriteLine(s.Length());` COMPILED with two `internal:` warnings
// (#605 permissive acceptance, #2477 degraded slot) and printed NOTHING.
// The #7373 fix makes the call a compile error; this file pins the rule-1
// NOTE that a same-named extension exists and was not consulted, on both
// the `.` and the `?.` spelling, and that the extension's 42 is never
// reached.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The body always sits on line 8.
std::string prog(const std::string& body) {
    return "module M;\n"                                                   // 1
           "extension string public method Length(): int => 42; "
           "public method IsAscii(): bool => false; "
           "public method Tag(): string => \"T\"; end;\n"                  // 2
           "class C\n"                                                     // 3
           "  public static method Main(): int\n"                          // 4
           "    var s := \"abc\";\n"                                       // 5
           "    var t: string? := \"xy\";\n"                               // 6
           "    var u := 0;\n"                                             // 7
           "    " + body + "\n"                                            // 8
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

const char* const kNote =
    "; note: an extension member named `";
const char* const kNoteTail =
    "` also matches this receiver but was not considered";

// The AOT oracle reports each diagnostic as `<file>:<line>:<col>: <text>`.
bool errorOnLineWith(const JitResult& r, const std::string& a,
                     const std::string& b, int line) {
    const std::string at = ".bzl:" + std::to_string(line) + ":";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(at) != std::string::npos &&
            d.message.find(a) != std::string::npos &&
            d.message.find(b) != std::string::npos)
            return true;
    return false;
}

void expectRejectedWithNote(const std::string& body,
                            const std::string& member) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(errorOnLineWith(
        r, "`" + member + "` is a property of `string`, not a method",
        std::string(kNote) + member + kNoteTail, 8));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("42") == std::string::npos);
}

void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// The issue's own repro.
BEZEL_TEST(sema_string_property_extension_7477,
           length_call_with_same_named_extension_rejected_with_note_spec_26_5_rule_1) {
    expectRejectedWithNote("Console.WriteLine(s.Length());", "Length");
}

BEZEL_TEST(sema_string_property_extension_7477,
           literal_receiver_length_call_rejected_with_note_spec_26_5_rule_1) {
    expectRejectedWithNote("Console.WriteLine(\"abc\".Length());", "Length");
}

BEZEL_TEST(sema_string_property_extension_7477,
           is_ascii_call_with_same_named_extension_rejected_with_note_spec_26_5_rule_1) {
    expectRejectedWithNote("Console.WriteLine($\"{s.IsAscii()}\");",
                           "IsAscii");
}

// The `?.` twin: the check runs before resolveNullCondMember's extension
// lookup, so the extension is not bound either.
BEZEL_TEST(sema_string_property_extension_7477,
           null_conditional_length_call_rejected_with_note_spec_26_5_rule_1) {
    expectRejectedWithNote(
        "var n: int? := t?.Length(); Console.WriteLine(n ?? -1);", "Length");
}

// CONTROLS. The bare read is the §9.6 property, not the extension (3, not
// 42); an extension under a name §9.6 does not declare is still reached
// (§26.5 rule 2).
BEZEL_TEST(sema_string_property_extension_7477,
           bare_read_is_the_property_not_the_extension_spec_26_5_rule_1) {
    expectStdout("Console.WriteLine(s.Length); Console.WriteLine(s.IsAscii);",
                 "3\nTrue\n");
}

BEZEL_TEST(sema_string_property_extension_7477,
           extension_under_an_undeclared_name_still_compiles_spec_26_5_rule_2) {
    expectStdout("Console.WriteLine(s.Tag());", "T\n");
}
