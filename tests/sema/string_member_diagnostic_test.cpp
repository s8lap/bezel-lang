// Spec-derived tests for string member access in sema (§4.6, stdlib §9.6).
//
// CG27a: `.Length` and `.IsAscii` are now recognized intrinsic members —
// sema accepts them and sets their expr type (int and bool respectively).
// Unknown string members still produce a diagnostic. String variables
// and literals are legal with or without member access.

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

// §4.6, stdlib §9.6: `.Length` is a known intrinsic → sema accepts it (no error).
BEZEL_TEST(sema_string_member, length_is_accepted) {
    auto r = check("var s := \"hello\"; return s.Length;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An unknown string member still produces a diagnostic.
BEZEL_TEST(sema_string_member, unknown_member_is_rejected) {
    auto r = check("var s := \"hello\"; return s.NonExistent;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// String variables themselves are still valid; no member access is fine.
BEZEL_TEST(sema_string_member, string_variable_no_member_is_ok) {
    auto r = check("var s := \"hello\"; return 42;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Member access on a non-string primitive (e.g. int) is not mis-diagnosed.
BEZEL_TEST(sema_string_member, int_member_access_different_error) {
    auto r = check("var n := 5; return n;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
