// Spec-derived sema tests for fix #746 (R41 cluster D) — `string + non-string`.
//
// Authority: §4.6 — "`s + t` concatenates two strings. The compiler does not
// implicitly convert other types to `string`; use interpolation or
// `.ToString()`." A mixed `string + <non-string>` was silently converted
// (`"x=" + 7`), IR-failed (`"v=" + 1.5` → `sitofp ptr`), or returned garbage
// (`42 + " h"`). The fix requires both `+` operands to be `string`.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasConcatError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("both operands to be `string`") != std::string::npos &&
            d.message.find("4.6") != std::string::npos)
            return true;
    return false;
}

std::string mainOf(const char* expr) {
    return std::string(
        "module M; static class F public static method Main(): int "
        "  var s := ") + expr + "; return s.Length; end Main; end F;";
}

}  // namespace

// §4.6: `string + int` is rejected.
BEZEL_TEST(string_concat_746, string_plus_int_rejected) {
    BEZEL_EXPECT_TRUE(hasConcatError(parseSemaAll(mainOf("\"x=\" + 7"))));
}

// §4.6: `string + double` (was an IR_FAIL) is rejected.
BEZEL_TEST(string_concat_746, string_plus_double_rejected) {
    BEZEL_EXPECT_TRUE(hasConcatError(parseSemaAll(mainOf("\"v=\" + 1.5"))));
}

// §4.6: the reverse `int + string` is rejected.
BEZEL_TEST(string_concat_746, int_plus_string_rejected) {
    BEZEL_EXPECT_TRUE(hasConcatError(parseSemaAll(mainOf("42 + \" h\""))));
}

// §4.6: `string + char` is rejected.
BEZEL_TEST(string_concat_746, string_plus_char_rejected) {
    BEZEL_EXPECT_TRUE(hasConcatError(parseSemaAll(mainOf("\"c=\" + 'Z'"))));
}

// §4.6: `string + string` is valid concatenation — no error.
BEZEL_TEST(string_concat_746, string_plus_string_ok) {
    BEZEL_EXPECT_FALSE(hasConcatError(parseSemaAll(mainOf("\"a\" + \"b\""))));
}

// §4.6: numeric `+` is unaffected.
BEZEL_TEST(string_concat_746, numeric_plus_ok) {
    BEZEL_EXPECT_FALSE(hasConcatError(parseSemaAll(
        "module M; static class F public static method Main(): int "
        "  return 3 + 4; end Main; end F;")));
}
