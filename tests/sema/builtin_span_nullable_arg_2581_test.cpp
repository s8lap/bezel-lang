// Spec-derived regression test for issue #2581.
//
// §7 (Nullability) / stdlib §3.3 (ReadOnlyStringSpan) + §9.6 (String), v0.236:
// the same #2315 §7 hole — a builtin call with no MethodSymbol skips the
// per-argument assignability check — on three adjacent builtin string surfaces
// that #2315's primitive-string INSTANCE gate does NOT reach:
//   (1) ReadOnlyStringSpan instance methods (IndexOf/Contains/StartsWith/
//       EndsWith/SequenceEqual, stdlib §3.3) — a NAMED receiver type, so the
//       gate's Primitive-String VALUE check misses it;
//   (2) the STATIC `string.Compare(a: string, b: string)` call — the callee
//       target is the type name `string`, which carries no VALUE exprType;
//   (3) `string.Insert(startIndex: int, value: string)` pos-0 — absent from the
//       intPos set, so an un-narrowed `int?` startIndex slipped through;
//   (4) a null-conditional instance call `s?.IndexOf(n)` (NullCondMemberExpr) —
//       a separate Expr kind the MemberAccessExpr-only gate missed.
// In each, an un-narrowed `string?`/`char?`/`int?` argument was silently
// accepted. The fix extends the §7 assignability check to all four paths.
//
// This is a REJECTION fix: each negative case asserts on diagnostic TEXT (not
// the coarse rejects() helper, which cannot distinguish a genuine reject from a
// runtime SIGSEGV synthesized as an Error). The positive cases guard against
// false rejection of working code.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>
#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §12/§7 "nullable -> non-nullable" assignability diagnostic fragment
// shared by the user-method path and (after #2315/#2581) the builtin paths.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2581 / stdlib §3.3: `sp.IndexOf(n)` on a ReadOnlyStringSpan with an
// un-narrowed `string?` rejects. Without the fix this emitted NO diagnostic.
BEZEL_TEST(sema_span_nullable_arg_2581,
           span_indexof_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    return sp.IndexOf(N()); "  // N(): string? — un-narrowed
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "ReadOnlyStringSpan.IndexOf"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§7)"));
}

// #2581 / stdlib §3.3: the ReadOnlyStringSpan search/compare family — Contains,
// StartsWith, EndsWith, SequenceEqual — all reject an un-narrowed `string?`.
BEZEL_TEST(sema_span_nullable_arg_2581,
           span_search_family_rejects_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(): {RET} "
        "    var sp := \"hello\".AsSpan(); "
        "    return sp.{METHOD}(N()); "
        "  end T; "
        "end C;";
    struct Case { const char* method; const char* ret; };
    for (const auto& c : std::initializer_list<Case>{
            {"Contains",      "bool"},
            {"StartsWith",    "bool"},
            {"EndsWith",      "bool"},
            {"SequenceEqual", "bool"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{RET}"), 5, c.ret);
        src.replace(src.find("{METHOD}"), 8, c.method);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        std::string meth =
            std::string("ReadOnlyStringSpan.") + c.method;
        BEZEL_EXPECT_TRUE(hasError(r, meth.c_str()));
    }
}

// #2581 / stdlib §3.3: `sp.Contains('x')` char overload rejects an un-narrowed
// `char?` (the IndexOf/Contains char|string family picks the char overload by
// the argument's own kind).
BEZEL_TEST(sema_span_nullable_arg_2581,
           span_contains_rejects_unnarrowed_nullable_char) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NC(): char? => null; "
        "  public static method T(): bool "
        "    var sp := \"hello\".AsSpan(); "
        "    return sp.Contains(NC()); "  // NC(): char? — un-narrowed
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "ReadOnlyStringSpan.Contains"));
}

// #2581 / stdlib §9.6: static `string.Compare(n, "x")` with an un-narrowed
// `string?` first argument rejects. The callee target is the type name, which
// #2315's instance-receiver VALUE gate does not reach.
BEZEL_TEST(sema_span_nullable_arg_2581,
           static_string_compare_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(): int "
        "    return string.Compare(N(), \"x\"); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "string.Compare"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§7)"));
}

// #2581 / stdlib §9.6: static `string.Compare("a", n)` rejects on the SECOND
// argument too (both a/b are non-nullable); the 3-arg `ignoreCase` overload is
// the same shape.
BEZEL_TEST(sema_span_nullable_arg_2581,
           static_string_compare_second_arg_and_3arg_reject) {
    {
        auto r = parseSemaAll(
            "module M; "
            "class C "
            "  private static method N(): string? => null; "
            "  public static method T(): int "
            "    return string.Compare(\"a\", N()); "
            "  end T; "
            "end C;");
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    }
    {
        auto r = parseSemaAll(
            "module M; "
            "class C "
            "  private static method N(): string? => null; "
            "  public static method T(): int "
            "    return string.Compare(N(), \"x\", true); "
            "  end T; "
            "end C;");
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    }
}

// #2581 / stdlib §9.6: `s.Insert(n, "X")` rejects on an un-narrowed `int?`
// startIndex (pos 0) — `Insert` was absent from the intPos set.
BEZEL_TEST(sema_span_nullable_arg_2581,
           string_insert_rejects_unnarrowed_nullable_int_startindex) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NI(): int? => null; "
        "  public static method T(): string "
        "    var s := \"abc\"; "
        "    return s.Insert(NI(), \"X\"); "  // NI(): int? — un-narrowed
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "string.Insert"));
}

// #2581 / stdlib §9.6 + §3.3: a null-conditional instance call `s?.IndexOf(n)`
// (NullCondMemberExpr — a separate Expr kind) rejects an un-narrowed `string?`.
// The MemberAccessExpr-only gate missed this path.
BEZEL_TEST(sema_span_nullable_arg_2581,
           nullcond_indexof_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(s: string?): int "
        "    return s?.IndexOf(N()); "  // N(): string? — un-narrowed, on `?.`
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "string.IndexOf"));
}

// ---- Positive controls (guard against false rejection of working code) ----

// #2581 / §7.4: a NARROWED `string?` (after a null-check) IS assignable to the
// non-nullable `string` parameter on a ReadOnlyStringSpan method.
BEZEL_TEST(sema_span_nullable_arg_2581,
           span_indexof_accepts_narrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var n: string? := \"ll\"; "
        "    if n = null then return -1; end; "  // narrows n non-null below
        "    return sp.IndexOf(n); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2581 / stdlib §3.3: plain non-nullable `string`/`char` arguments on every
// ReadOnlyStringSpan search/compare method compile unchanged.
BEZEL_TEST(sema_span_nullable_arg_2581,
           span_search_family_accepts_plain_args) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    sp.IndexOf('l'); "
        "    sp.IndexOf(\"ll\"); "
        "    sp.Contains('l'); "
        "    sp.Contains(\"ll\"); "
        "    sp.StartsWith(\"he\"); "
        "    sp.EndsWith(\"lo\"); "
        "    sp.SequenceEqual(\"hello\"); "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2581 / stdlib §3.3: a ReadOnlyStringSpan-TYPED operand (the Named overload,
// outside the primitive nullable hole) compiles unchanged.
BEZEL_TEST(sema_span_nullable_arg_2581,
           span_accepts_span_typed_operand) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var sp2 := \"world\".AsSpan(); "
        "    sp.IndexOf(sp2); "
        "    sp.Contains(sp2); "
        "    sp.StartsWith(sp2); "
        "    sp.SequenceEqual(sp2); "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2581 / stdlib §9.6: static `string.Compare` with plain `string` args, and a
// NARROWED `string?`, compile unchanged.
BEZEL_TEST(sema_span_nullable_arg_2581,
           static_string_compare_accepts_plain_and_narrowed) {
    {
        auto r = parseSemaAll(
            "module M; "
            "class C "
            "  public static method T(): int "
            "    return string.Compare(\"a\", \"b\"); "
            "  end T; "
            "end C;");
        BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    }
    {
        auto r = parseSemaAll(
            "module M; "
            "class C "
            "  public static method T(): int "
            "    var n: string? := \"a\"; "
            "    if n = null then return -1; end; "
            "    return string.Compare(n, \"b\"); "
            "  end T; "
            "end C;");
        BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    }
}

// #2581 / stdlib §9.6: `IsNullOrWhiteSpace(value: string?)` declares a
// NULLABLE parameter, so an un-narrowed `string?` stays admissible — the fix
// must not reject it.
//
// fix #3854 (stdlib §9.6, v0.290): this case originally covered
// `IsNullOrWhiteSpace`'s sibling `string.IsNullOrEmpty(value: string?)` too
// (`return string.IsNullOrEmpty(N()) or string.IsNullOrWhiteSpace(N());`).
// `IsNullOrEmpty` was impl-ahead-of-spec surface, retired at v0.290
// (proposal 0123 R6) — deleted from the compiler by #3854, not merely
// undocumented — so the case now exercises `IsNullOrWhiteSpace` alone; the
// nullable-argument admissibility it tests is unchanged.
BEZEL_TEST(sema_span_nullable_arg_2581,
           static_isnullorwhitespace_accepts_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(): bool "
        "    return string.IsNullOrWhiteSpace(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2581 / stdlib §9.6: a plain `int` startIndex on `string.Insert` compiles
// unchanged (the intPos addition must not false-reject).
BEZEL_TEST(sema_span_nullable_arg_2581,
           string_insert_accepts_plain_int) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): string "
        "    var s := \"abc\"; "
        "    return s.Insert(1, \"X\"); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2581 / §9.6: a null-conditional call `s?.IndexOf("x")` with a PLAIN string
// argument compiles unchanged (the gate checks the ARG, not the receiver).
BEZEL_TEST(sema_span_nullable_arg_2581,
           nullcond_indexof_accepts_plain_arg) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(s: string?): int "
        "    var n := s?.IndexOf(\"x\"); "  // plain string arg on `?.` call
        "    if n = null then return -1; end; "
        "    return n; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2581 discriminator: a USER static `Compare(a: string?, b: string?)` and a
// user instance `Contains(value: string?)` take nullable params and must NOT be
// touched by the builtin gates (no false-reject / no double-diagnose).
BEZEL_TEST(sema_span_nullable_arg_2581,
           user_compare_and_contains_not_gated) {
    auto r = parseSemaAll(
        "module M; "
        "class UserThing "
        "  public static method Compare(a: string?, b: string?): int => 0; "
        "  public method Contains(value: string?): bool => false; "
        "end UserThing; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(): int "
        "    var u1 := UserThing.Compare(N(), N()); "  // user method, nullable
        "    return u1; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}
