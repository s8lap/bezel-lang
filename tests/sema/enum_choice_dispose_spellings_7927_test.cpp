// Regression tests for #7927 (§8.7, v0.350): `e?.Dispose()` on a nullable enum
// or choice and the type-name call `E.Dispose()` compiled.
//
// §8.7 (v0.350): "a call in any spelling (`x.Dispose()`, `x?.Dispose()`,
// `x!.Dispose()`) ... [is a] compile error" on a type with no `Dispose`
// member; "Static classes, records, enums, choices ... do not implement
// `IDisposable`".
//
// ROOT CAUSE: typeCall (ExprTypecheck_Calls.cpp) rejected `Dispose` only on a
// MemberAccessExpr callee whose receiver value is an enum/choice; the
// NullCondMemberExpr callee (`?.`) and the type-name receiver had no arm.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

void expectRejected(const std::string& body, const char* needle) {
    auto r = runAot(
        "module M; "
        "public enum E Red, Green; end E; "
        "public choice Ch A, B; end Ch; "
        "public class Program "
        "  public static method Main() " + body + " end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""8.7"));
}

}  // namespace

BEZEL_TEST(enum_choice_dispose_spellings_7927, null_conditional_nullable_enum_rejected_8_7) {
    expectRejected("var e: E? := E.Red; e?.Dispose();", "`Dispose` is not admitted on an enum value");
}

BEZEL_TEST(enum_choice_dispose_spellings_7927, null_conditional_nullable_choice_rejected_8_7) {
    expectRejected("var c: Ch? := Ch.A; c?.Dispose();", "`Dispose` is not admitted on a choice value");
}

BEZEL_TEST(enum_choice_dispose_spellings_7927, type_name_enum_call_rejected_8_7) {
    expectRejected("E.Dispose();", "`Dispose` is not admitted on an enum value");
}

BEZEL_TEST(enum_choice_dispose_spellings_7927, type_name_choice_call_rejected_8_7) {
    expectRejected("Ch.Dispose();", "`Dispose` is not admitted on a choice value");
}

BEZEL_TEST(enum_choice_dispose_spellings_7927, bang_spelling_nullable_enum_rejected_8_7) {
    expectRejected("var e: E? := E.Red; e!.Dispose();", "`Dispose` is not admitted on an enum value");
}

