// Spec-derived regression test for issue #2315.
//
// §7 (Nullability) / stdlib §9.6 (String), v0.236: a builtin primitive
// `string` instance method that declares a NON-nullable `string`/`char`
// parameter must reject an UN-narrowed `string?` (or `char?`) argument at
// compile time — the same §7 assignability rule a user-defined
// `method Take(x: string)` enforces. Previously the builtin string call path
// skipped the per-argument assignability check (no MethodSymbol), silently
// accepting the un-narrowed nullable and reaching codegen, where the runtime
// helper's accidental null guard converted the type error into a "not found"
// / wrong answer.
//
// This is a REJECTION fix, so each negative case asserts on diagnostic TEXT
// (not the coarse rejects() helper, which cannot distinguish a genuine reject
// from a runtime SIGSEGV synthesized as an Error). The positive cases guard
// against false rejection of working code.

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
// shared by the user-method path and (after #2315) the builtin-string path.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2315 / §12 / stdlib §9.6: `s.IndexOf(n)` with an un-narrowed `string?`
// rejects. Without the fix this emitted NO diagnostic (silently accepted).
BEZEL_TEST(sema_string_nullable_arg_2315,
           builtin_indexof_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(): int "
        "    var s := \"hello\"; "
        "    return s.IndexOf(N()); "  // N(): string? — un-narrowed
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "string.IndexOf"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§7)"));
}

// #2315 / stdlib §9.6: `s.Contains(n)`, `s.StartsWith(n)`, `s.EndsWith(n)`,
// `s.LastIndexOf(n)` — the rest of the search family — all reject.
BEZEL_TEST(sema_string_nullable_arg_2315,
           builtin_search_family_rejects_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(): {RET} "
        "    var s := \"hello\"; "
        "    return s.{METHOD}(N()); "
        "  end T; "
        "end C;";
    struct Case { const char* method; const char* ret; };
    for (const auto& c : std::initializer_list<Case>{
            {"Contains",   "bool"},
            {"StartsWith", "bool"},
            {"EndsWith",   "bool"},
            {"LastIndexOf","int"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{RET}"), 5, c.ret);
        src.replace(src.find("{METHOD}"), 8, c.method);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        std::string meth = std::string("string.") + c.method;
        BEZEL_EXPECT_TRUE(hasError(r, meth.c_str()));
    }
}

// #2315 / stdlib §9.6: `s.Replace(n, "y")` rejects on the nullable
// `oldValue` (first string parameter position).
BEZEL_TEST(sema_string_nullable_arg_2315,
           builtin_replace_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(): string "
        "    var s := \"hello\"; "
        "    return s.Replace(N(), \"x\"); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "string.Replace"));
}

// #2315 / §7.4: a NARROWED `string?` (after a null-check guard) IS assignable
// to the non-nullable `string` parameter — only the un-narrowed form is the
// hole. This positive case must compile with NO nullable-assignability error
// (guards against a false-reject regression in the narrowing-aware path).
BEZEL_TEST(sema_string_nullable_arg_2315,
           builtin_indexof_accepts_narrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var s := \"hello\"; "
        "    var n: string? := \"ll\"; "
        "    if n = null then return -1; end; "  // narrows n non-null below
        "    return s.IndexOf(n); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2315 / stdlib §9.6: a plain non-nullable `string` argument compiles
// unchanged (positive baseline).
BEZEL_TEST(sema_string_nullable_arg_2315,
           builtin_indexof_accepts_plain_string) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var s := \"hello\"; "
        "    var sub := \"ll\"; "
        "    return s.IndexOf(sub); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2315 / §11.2 (parameter-nullability widening): `s.Equals(other: string?)`
// deliberately declares a NULLABLE parameter, so a `string?` argument to
// `Equals` stays admissible — the fix must not reject it.
BEZEL_TEST(sema_string_nullable_arg_2315,
           builtin_equals_accepts_nullable_per_11_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(): bool "
        "    var s := \"hello\"; "
        "    return s.Equals(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}
