// Spec-derived tests for issue #1597 — a reference `out`-parameter's
// nullable-strip previously hid `T?` inference for a USER-declared method's
// inline `out var` (and `out owned var`) recipient. This is a follow-up to
// #1336, which wired nullable inference for the inline out-var recipient of
// BUILTIN `Try*` methods only (Stack.TryPop, Dictionary.TryGetValue, etc.);
// #1597 covers the USER-declared-method out-param path that #1336's fix left
// untouched.
//
// §22.4 "Inline `out` declarations": "`out var name` declares a new variable
// inline. Its type is inferred from the method's `out` parameter
// declaration" — with no carve-out dropping nullability from that inferred
// type. §7.9 ("Definite assignment for non-nullable fields")'s default-
// initialization subsection states the three-way split a `var` declaration
// must respect: value-typed (default value), nullable (default `null`),
// non-nullable reference (must be assigned before read) — the SAME split
// that determines whether a *declared* `T?` local is nullable downstream.
// An inline `out var` is exactly such a declaration: a `T?` out param must
// produce a `T?` local, so a later unguarded read is still subject to
// §7.4's nullable-member-access rule; only a genuinely non-nullable out
// param may infer a non-nullable local.
//
// Before the fix, `src/sema/ExprTypecheck_Exprs.cpp`'s inline out-var
// registration (visitChildren, CallExpr case) unconditionally cleared
// `declType->nullable` for BOTH the plain `out var` and `out owned var`
// forms whenever the callee's declared out-parameter type was nullable,
// reasoning (per the removed comment) that the strip was
// "representationally harmless for a reference type" — true only for
// CODEGEN's pointer lowering (a nullable class type and its non-nullable
// form share the same `ptr` representation), not for the type checker,
// which then treated the recipient as provably non-null forever after.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
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

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// -----------------------------------------------------------------------
// Plain `out var` — user-declared method.
// -----------------------------------------------------------------------

// §22.4/§7.9 (#1597): a USER-declared method's nullable out param
// (`out v: string?`) bound with an inline `out var s` must type `s` as
// `string?` — a direct (unguarded) `s.Length` after the call is a §7.4
// nullable-member-access error, exactly as it would be for a directly
// declared `var s: string? := null;`.
BEZEL_TEST(user_out_var_1597,
           nullable_out_param_out_var_infers_nullable_reject_unguarded_spec_22_4_7_9) {
    auto r = parseSemaAll(
        "module M; "
        "class Finder "
        "  public static method TryGet(out v: string?): bool "
        "    v := null; return false; "
        "  end TryGet; "
        "  public static method Run(): int "
        "    var ok := Finder.TryGet(out var s); "
        "    return s.Length; "
        "  end Run; "
        "end Finder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "on a nullable value requires narrowing"));
}

// Control (no over-widening): a NON-nullable out param (`out v: string`)
// still infers plain `string` — the unguarded read stays ACCEPTED, exactly
// as before the fix.
BEZEL_TEST(user_out_var_1597,
           nonnullable_out_param_out_var_infers_nonnullable_accept_spec_22_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Finder "
        "  public static method TryGetNonNull(out v: string): bool "
        "    v := \"hi\"; return true; "
        "  end TryGetNonNull; "
        "  public static method Run(): int "
        "    var ok := Finder.TryGetNonNull(out var s); "
        "    return s.Length; "
        "  end Run; "
        "end Finder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4: the nullable out-var's type is genuinely narrowable — a guarded
// `s <> null` check ACCEPTS the subsequent `s.Length`, proving `s` is really
// typed `string?` (narrowing has a nullable type to narrow FROM), not just
// left untyped/unchecked.
BEZEL_TEST(user_out_var_1597,
           nullable_out_var_narrows_after_null_check_accepted_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Finder "
        "  public static method TryGet(out v: string?): bool "
        "    v := null; return false; "
        "  end TryGet; "
        "  public static method Run(): int "
        "    var ok := Finder.TryGet(out var s); "
        "    if s <> null then return s.Length; end; "
        "    return 0; "
        "  end Run; "
        "end Finder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// -----------------------------------------------------------------------
// `out owned var` — the identical strip existed in this sibling inline
// form (same CallExpr-handling function, same reasoning, same fix).
// -----------------------------------------------------------------------

// §22.4: a nullable `out owned conn: Cn?` bound via the inline
// `out owned var c` form must type `c` as `Cn?` — unguarded `c.Use()` is
// REJECTED (§7.4).
BEZEL_TEST(user_out_var_1597,
           out_owned_var_nullable_infers_nullable_reject_unguarded_spec_22_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Cn public method Use(): int => 1; end Cn; "
        "class Pool "
        "  public method TryTake(out owned conn: Cn?): bool "
        "    conn := new Cn(); return true; "
        "  end TryTake; "
        "  public static method Run(): int "
        "    var p := new Pool(); "
        "    var ok := p.TryTake(out owned var c); "
        "    return c.Use(); "
        "  end Run; "
        "end Pool;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "on a nullable value requires narrowing"));
}

// Control: a NON-nullable `out owned conn: Cn` still infers plain `Cn` —
// unguarded `c.Use()` stays ACCEPTED.
BEZEL_TEST(user_out_var_1597,
           out_owned_var_nonnullable_infers_nonnullable_accept_spec_22_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Cn public method Use(): int => 1; end Cn; "
        "class Pool "
        "  public method TryTakeNonNull(out owned conn: Cn): bool "
        "    conn := new Cn(); return true; "
        "  end TryTakeNonNull; "
        "  public static method Run(): int "
        "    var p := new Pool(); "
        "    var ok := p.TryTakeNonNull(out owned var c); "
        "    return c.Use(); "
        "  end Run; "
        "end Pool;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
