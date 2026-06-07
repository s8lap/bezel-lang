// Spec-derived tests for issue #491 — calling an unknown static method on a
// primitive type must produce a clean compile error, not a crash.
//
// Spec basis. Primitive-type static members (§3.4 / stdlib §5.2, §9.5, §9.6)
// are a fixed, closed set recognised by the sema primitive-static handler in
// ExprTypecheck_Calls.cpp (~line 2396).  Any name NOT in that set must be
// diagnosed as an error at sema time; if sema falls through without setting an
// expr type, the codegen phase receives an untyped member-access node and
// crashes (SIGABRT).
//
// This file tests both the negative cases (unknown names → must error) and the
// positive controls (all recognised names → must NOT error).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// Helper: wrap a method body inside a minimal module/class and run sema.
bezel::test::SemaResult checkBody(std::string_view body) {
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

} // namespace

// ===========================================================================
// NEGATIVE — unknown primitive static methods must produce a compile error.
// Before fix #491 these all triggered a SIGABRT in codegen.
// ===========================================================================

// §3.4 / stdlib §9.5: char has no static method `Repeat`.  The real API is
// string.Repeat(c: char, count: int) at stdlib §9.6.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, char_repeat_is_rejected) {
    auto r = checkBody("var rep := char.Repeat('x', 3); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §3.4 / stdlib §9.5: `char.Bogus` is not listed anywhere in the stdlib.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, char_bogus_is_rejected) {
    auto r = checkBody("var x := char.Bogus('a'); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §3.4 / stdlib §5.2: int has no static method `Frobnicate`.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, int_frobnicate_is_rejected) {
    auto r = checkBody("var x := int.Frobnicate(42); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §3.4 / stdlib §7.1: double has no static method `Xyz`.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, double_xyz_is_rejected) {
    auto r = checkBody("var x := double.Xyz(1.0); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §3.4 / stdlib §5.2: long has no static method `Quux`.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, long_quux_is_rejected) {
    auto r = checkBody("var x := long.Quux(1); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// POSITIVE controls — recognized primitive static members must NOT produce a
// compile error.  These must pass both before and after the fix.
// ===========================================================================

// §3.4 / stdlib §9.5: char.IsDigit(c: char): bool — recognized.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, char_isdigit_is_accepted) {
    auto r = checkBody("var b := char.IsDigit('5'); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §3.4 / stdlib §9.5: char.ToUpper(c: char): char — recognized.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, char_toupper_is_accepted) {
    auto r = checkBody("var c := char.ToUpper('a'); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §3.4 / stdlib §9.5: char.ToString(c: char): string — recognized.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, char_tostring_is_accepted) {
    auto r = checkBody("var s := char.ToString('z'); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §3.4 / stdlib §5.2: int.Parse(s: string): int — recognized.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, int_parse_is_accepted) {
    auto r = checkBody("var n := int.Parse(\"42\"); return n;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §3.4 / stdlib §7.1: double.Parse(s: string): double — recognized.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, double_parse_is_accepted) {
    auto r = checkBody("var d := double.Parse(\"3.14\"); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §3.4 / stdlib §9.6: string.Repeat(c: char, count: int): string — the real
// Repeat API (on string, not char).  Must be accepted without error.
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, string_repeat_is_accepted) {
    auto r = checkBody("var s: string := string.Repeat('x', 3); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// stdlib §9: Math.Abs(x: double): double — a Math builtin, not a primitive
// static, but must remain accepted (control against over-rejection).
BEZEL_TEST(sema_unknown_primitive_method_spec3_4, math_abs_is_accepted) {
    auto r = checkBody("var a := Math.Abs(0 - 5); return a;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
