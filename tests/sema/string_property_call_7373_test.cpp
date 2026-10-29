// Spec-derived regression test for issue #7373 (and the no-extension
// `"abc".Length()` shape the C2-08 lane measured with it).
//
// stdlib §9.6 (v0.350) declares three NAMED properties on `String`:
//
//   public pure readonly property Length: int;
//   public pure readonly property IsEmpty: bool;
//   public pure readonly property IsAscii: bool;
//
// spec §26.5 rule 1 (v0.350): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` — of any arity, applicability,
// or accessibility — instance resolution is committed and extension blocks
// are **never consulted** for that name on that receiver. If the committed
// instance lookup then fails …, the call is a compile error citing the
// instance member(s)". The member is a property, so `s.IsAscii()` is a call
// THROUGH the property's value; spec §27.1 (v0.350) names "a parameter,
// local, field, or property of function type" as the function-typed callees,
// and `bool` / `int` is not a function type (§27.7).
//
// THE DEFECT — a SILENT-ACCEPTANCE INVERSION. MEASURED on r30-c1 29864453:
//
//   Console.WriteLine($"{s.IsAscii()}");  -> COMPILED with two `internal:`
//       warnings (permissive acceptance #605, then the #1386 placeholder) and
//       printed `False` for "abc"
//   Console.WriteLine(s.Length());        -> COMPILED with `internal:` warnings
//       and printed NOTHING (not even the line terminator, #3079)
//   var n: int? := s?.Length();           -> COMPILED, degraded the slot (#2434)
//       and printed -1 for a non-null "abc"
//
// THE ROWS. The rejection rows assert the SPECIFIC diagnostic at the call's
// own source line. The CONTROL rows are load-bearing for the over-reject
// direction: the bare property reads, genuine §9.6 methods, `Array<T>`'s
// `Contains`, a call through a FUNCTION-typed property (§27.1 admits it), a
// user extension under a name §9.6 does not declare, and an extension over a
// user type must all still compile and print their values.
#include <cstdint>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The body always sits on line 9.
std::string prog(const std::string& body) {
    return "module M;\n"                                               // 1
           "extension string public method Tag(): string => \"T\"; end;\n"  // 2
           "class U public property N: int := 3; "
           "public property F: (int) -> int := lambda(x: int) => x + 1; "
           "end U; extension U public method Twice(): int => this.N * 2; "
           "end;\n"                                                    // 3
           "class C\n"                                                 // 4
           "  public static method Main(): int\n"                      // 5
           "    var s := \"abc\";\n"                                   // 6
           "    var t: string? := \"xy\";\n"                           // 7
           "    var a: Array<int> := [1,2,3];\n"                       // 8
           "    " + body + "\n"                                        // 9
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The AOT oracle reports each diagnostic as `<file>:<line>:<col>: <text>`, so
// the line is read from the message's own `.bzl:<line>:` prefix.
bool errorOnLine(const JitResult& r, const std::string& needle,
                 std::uint32_t line) {
    const std::string at = ".bzl:" + std::to_string(line) + ":";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos &&
            d.message.find(at) != std::string::npos)
            return true;
    return false;
}

void expectPropertyCallRejected(const std::string& body,
                                const std::string& member,
                                const std::string& type) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    const std::string needle = "`" + member +
                               "` is a property of `string`, not a method "
                               "(stdlib \xC2\xA7""9.6: `" + member + ": " +
                               type + "`)";
    BEZEL_EXPECT_TRUE(errorOnLine(r, needle, 9));
}

void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ── the DEFECT: the call spelling of a §9.6 property ─────────────────────

// The issue's own repro (inside an interpolation).
BEZEL_TEST(sema_string_property_call_7373,
           is_ascii_call_in_interpolation_rejected_spec_26_5_rule_1) {
    expectPropertyCallRejected("Console.WriteLine($\"{s.IsAscii()}\");",
                               "IsAscii", "bool");
}

BEZEL_TEST(sema_string_property_call_7373,
           length_call_on_literal_rejected_spec_26_5_rule_1) {
    expectPropertyCallRejected("Console.WriteLine(\"abc\".Length());",
                               "Length", "int");
}

BEZEL_TEST(sema_string_property_call_7373,
           length_call_on_local_rejected_spec_26_5_rule_1) {
    expectPropertyCallRejected("var n: int := s.Length();", "Length", "int");
}

BEZEL_TEST(sema_string_property_call_7373,
           is_empty_call_rejected_spec_26_5_rule_1) {
    expectPropertyCallRejected("var b: bool := s.IsEmpty();", "IsEmpty",
                               "bool");
}

// The `?.` twin: it printed -1 for a non-null receiver.
BEZEL_TEST(sema_string_property_call_7373,
           null_conditional_length_call_rejected_spec_26_5_rule_1) {
    expectPropertyCallRejected(
        "var n: int? := t?.Length(); Console.WriteLine(n ?? -1);", "Length",
        "int");
}

// ── CONTROLS: what must still compile ───────────────────────────────────

BEZEL_TEST(sema_string_property_call_7373, bare_property_reads_still_compile) {
    expectStdout("Console.WriteLine(s.Length); Console.WriteLine(s.IsAscii); "
                 "Console.WriteLine(\"abcd\".Length); "
                 "Console.WriteLine(t?.Length ?? -1); "
                 "Console.WriteLine(s.Length.ToString());",
                 "3\nTrue\n4\n2\n3\n");
}

BEZEL_TEST(sema_string_property_call_7373, genuine_method_calls_still_compile) {
    expectStdout("Console.WriteLine(s.Substring(1)); "
                 "Console.WriteLine(a.Contains(1)); "
                 "Console.WriteLine(s.ToUpper());",
                 "bc\nTrue\nABC\n");
}

// §27.1 (v0.350): "a property of function type" is a callable callee.
BEZEL_TEST(sema_string_property_call_7373,
           function_typed_property_call_still_compiles_spec_27_1) {
    expectStdout("var u := new U(); Console.WriteLine(u.F(4));", "5\n");
}

BEZEL_TEST(sema_string_property_call_7373,
           extension_over_a_user_type_still_compiles_spec_26_5_rule_2) {
    expectStdout("var u := new U(); Console.WriteLine(u.Twice());", "6\n");
}

// §26.5 rule 2: §9.6 declares no `Tag`, so the extension is reached.
BEZEL_TEST(sema_string_property_call_7373,
           extension_under_an_undeclared_name_still_compiles_spec_26_5_rule_2) {
    expectStdout("Console.WriteLine(s.Tag());", "T\n");
}
