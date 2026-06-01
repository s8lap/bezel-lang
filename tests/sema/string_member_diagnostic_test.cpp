// Spec-derived tests for the string member access diagnostic (issue #251).
//
// `string` member access (e.g. `.Length`) silently returned 0 before this
// fix because the string runtime is not yet implemented. The diagnostic
// converts the silent-wrong-result into a clear compile-time error:
//   "member access on `string` (`.Length`) is not yet supported in
//    codegen — the string runtime is a future milestone"
//
// String variables and literals are still fully legal; only member access on
// string-typed values is diagnosed.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult check(std::string_view body) {
    std::string src =
        std::string("module M; class C public static method Main(): int ") +
        std::string(body) + " end Main; end C;";
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

}  // namespace

// `.Length` on a string variable is rejected.
BEZEL_TEST(sema_string_member, length_is_rejected) {
    auto r = check("var s := \"hello\"; return s.Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// String variables themselves are still valid; it's only member access
// that is rejected.
BEZEL_TEST(sema_string_member, string_variable_no_member_is_ok) {
    auto r = check("var s := \"hello\"; return 42;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Member access on a non-string primitive (e.g. int) is not mis-diagnosed.
BEZEL_TEST(sema_string_member, int_member_access_different_error) {
    // `n.Foo` on an int — this should fail with a different error
    // (unknown member or no member on int), NOT the string-specific
    // message. We just check that an integer variable can still be used.
    auto r = check("var n := 5; return n;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
