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

namespace {

// Full-module variant that allows top-level extension declarations (§26.5).
bezel::test::SemaResult checkMod(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

// fix #702 / §26.5: a user-defined extension method on `string` must resolve
// cleanly — the "not yet supported" catch-all must NOT fire before extension
// lookup is attempted.
BEZEL_TEST(sema_string_member, user_extension_on_string_resolves_clean_702) {
    auto r = checkMod(
        "module M; "
        "extension string "
        "  public method IsBlank(): bool => this.Length = 0; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        "    var b := s.IsBlank(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// fix #702 / §26.5 canary: a genuinely unknown string member (not an extension)
// must STILL produce a diagnostic — the fix must not weaken the error path.
BEZEL_TEST(sema_string_member, unknown_string_member_still_errors_after_702) {
    auto r = checkMod(
        "module M; "
        // Extension only covers IsBlank, NOT Typo.
        "extension string "
        "  public method IsBlank(): bool => this.Length = 0; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        "    var x := s.Typo; "   // genuinely unknown — should still error
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
