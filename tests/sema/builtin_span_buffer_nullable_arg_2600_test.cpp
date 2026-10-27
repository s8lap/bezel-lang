// Spec-derived regression test for issue #2600.
//
// §7 (Nullability) / stdlib §4.14 (OwningBuffer<T>, Span<T>, ReadOnlySpan<T>) +
// §4.15 (ArraySpan<T>, ReadOnlyArraySpan<T>), v0.236: a builtin span-BUFFER
// instance method that declares a NON-nullable `value: T` parameter —
// IndexOf/Contains on every span-buffer family, plus Fill on the writable forms
// (Span<T> for value T, ArraySpan<T> for all T) — must reject an UN-narrowed
// `T?` argument at compile time, the same §7 assignability rule a user-defined
// `method Take(x: int)` enforces. Previously the span-buffer call path skipped
// the per-argument assignability check (no MethodSymbol — synthetically typed
// in the CallExpr handler at ExprTypecheck_Calls.cpp:~6575), silently accepting
// the un-narrowed nullable and reaching codegen, where the equality-compare
// lowering tripped an LLVM ICmpInst assertion (`ICmpInst::AssertOK`: operand-
// type mismatch, exit 134) — the SAME #2582 failure mode, on a different
// family. The fix extends #2582's `checkBuiltinCollectionCallArgs` name list
// to cover the four span-buffer head names.
//
// §4.14 nullability split: class-T `Span<T>`/`ReadOnlySpan<T>` declare
// `IndexOf(value: T?)` / `Contains(value: T?)` — NULLABLE — so an un-narrowed
// `T?` is admissible there (the fix's spanBufferSkipClassT branch suppresses
// the check for that case). ArraySpan<T>/ReadOnlyArraySpan<T> declare
// `value: T` for ALL T (§4.15 — Array<T> slots do not wrap class T
// in T?), so they are covered unconditionally even for class T. The negative
// control `NegClassArraySpan` below confirms the class-T skip is scoped to
// Span/ReadOnlySpan and does NOT leak onto the ArraySpan family.
//
// This is a REJECTION fix: each negative case asserts on diagnostic TEXT (not
// the coarse rejects() helper — runAot's WIFSIGNALED path synthesizes an Error
// diagnostic for a runtime SIGSEGV/ICE too, so it cannot distinguish a genuine
// reject from the crash). The positive cases guard against false rejection of
// working code.

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
// shared by the user-method path and (after #2582/#2600) the builtin paths.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2600 / §7 / stdlib §4.15: the issue's exact repro shape — `sp.IndexOf(N())`
// with an un-narrowed `int?` on an `ArraySpan<int>`. Without the fix this
// ICE'd codegen (LLVM ICmpInst assertion, exit 134); with the fix it is a
// clean sema reject.
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           arrayspan_indexof_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(sp: ArraySpan<int>): int "
        "    return sp.IndexOf(N()); "  // N(): int? — un-narrowed
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "ArraySpan<T>.IndexOf"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§7)"));
}

// #2600 / stdlib §4.15: ArraySpan<T>.Contains and Fill also reject the
// un-narrowed `int?`. Fill is the value-T mutator the span-buffer family adds
// vs #2582's element-collection set.
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           arrayspan_contains_and_fill_reject_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(sp: ArraySpan<int>): {RET} "
        "    {STMT} "
        "  end T; "
        "end C;";
    struct Case { const char* stmt; const char* ret; const char* method; };
    for (const auto& c : std::initializer_list<Case>{
            {"return sp.Contains(N());", "bool", "Contains"},
            {"sp.Fill(N()); return 0;",  "int",  "Fill"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{RET}"), 5, c.ret);
        src.replace(src.find("{STMT}"), 6, c.stmt);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        std::string meth = std::string("ArraySpan<T>.") + c.method;
        BEZEL_EXPECT_TRUE(hasError(r, meth.c_str()));
    }
}

// #2600 / stdlib §4.15: ReadOnlyArraySpan<T>.IndexOf/Contains reject the
// un-narrowed `int?` (read-only family — Fill is not declared).
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           readonlyarrayspan_indexof_contains_reject_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(sp: ReadOnlyArraySpan<int>): {RET} "
        "    return sp.{METHOD}(N()); "
        "  end T; "
        "end C;";
    struct Case { const char* method; const char* ret; };
    for (const auto& c : std::initializer_list<Case>{
            {"IndexOf",  "int"},
            {"Contains", "bool"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{RET}"), 5, c.ret);
        src.replace(src.find("{METHOD}"), 8, c.method);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        std::string meth = std::string("ReadOnlyArraySpan<T>.") + c.method;
        BEZEL_EXPECT_TRUE(hasError(r, meth.c_str()));
    }
}

// #2600 / stdlib §4.14: value-T Span<T>.IndexOf/Contains/Fill reject the
// un-narrowed `int?` (the OWNING span over a value T).
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           span_valuet_indexof_contains_fill_reject_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(sp: Span<int>): {RET} "
        "    {STMT} "
        "  end T; "
        "end C;";
    struct Case { const char* stmt; const char* ret; const char* method; };
    for (const auto& c : std::initializer_list<Case>{
            {"return sp.IndexOf(N());",  "int",  "IndexOf"},
            {"return sp.Contains(N());", "bool", "Contains"},
            {"sp.Fill(N()); return 0;",  "int",  "Fill"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{RET}"), 5, c.ret);
        src.replace(src.find("{STMT}"), 6, c.stmt);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        std::string meth = std::string("Span<T>.") + c.method;
        BEZEL_EXPECT_TRUE(hasError(r, meth.c_str()));
    }
}

// #2600 / stdlib §4.14: value-T ReadOnlySpan<T>.IndexOf/Contains reject the
// un-narrowed `int?`.
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           readonlyspan_valuet_indexof_contains_reject_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(sp: ReadOnlySpan<int>): {RET} "
        "    return sp.{METHOD}(N()); "
        "  end T; "
        "end C;";
    struct Case { const char* method; const char* ret; };
    for (const auto& c : std::initializer_list<Case>{
            {"IndexOf",  "int"},
            {"Contains", "bool"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{RET}"), 5, c.ret);
        src.replace(src.find("{METHOD}"), 8, c.method);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        std::string meth = std::string("ReadOnlySpan<T>.") + c.method;
        BEZEL_EXPECT_TRUE(hasError(r, meth.c_str()));
    }
}

// #2600 / §7.4: a NARROWED `int?` (after a null-check guard) IS assignable to
// the non-nullable `int` parameter on every span-buffer family — only the
// un-narrowed form is the hole. Positive case guards against a false-reject
// regression in the narrowing-aware path.
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           span_buffer_indexof_accepts_narrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  public static method T(sp: {TYPE}<int>): int "
        "    var n: int? := 5; "
        "    if n = null then return -1; end; "  // narrows n non-null below
        "    var i := sp.IndexOf(n); "
        "    return i; "
        "  end T; "
        "end C;";
    for (const char* type : {"ArraySpan", "ReadOnlyArraySpan",
                             "Span", "ReadOnlySpan"}) {
        std::string src = tmpl;
        src.replace(src.find("{TYPE}"), 6, type);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    }
}

// #2600 / §7: a plain non-nullable `int` argument compiles unchanged on every
// span-buffer family (positive baseline — the fix must not reject working
// code). Covers IndexOf/Contains/Fill where admitted.
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           span_buffer_methods_accept_plain_int) {
    const char* tmpl =
        "module M; "
        "class C "
        "  public static method T(sp: {TYPE}<int>): int "
        "    {STMT} "
        "    return 0; "
        "  end T; "
        "end C;";
    struct Case { const char* type; const char* stmt; };
    for (const auto& c : std::initializer_list<Case>{
            {"ArraySpan",         "var b := sp.Contains(1); sp.Fill(2); "
                                  "var i := sp.IndexOf(1);"},
            {"ReadOnlyArraySpan", "var b := sp.Contains(1); "
                                  "var i := sp.IndexOf(1);"},
            {"Span",              "var b := sp.Contains(1); sp.Fill(2); "
                                  "var i := sp.IndexOf(1);"},
            {"ReadOnlySpan",      "var b := sp.Contains(1); "
                                  "var i := sp.IndexOf(1);"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{TYPE}"), 6, c.type);
        src.replace(src.find("{STMT}"), 6, c.stmt);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    }
}

// #2600 / stdlib §4.14: a CLASS-T `Span<T>` / `ReadOnlySpan<T>`
// declares `IndexOf(value: T?)` / `Contains(value: T?)` — NULLABLE — so an
// un-narrowed `K?` is admissible. The fix's spanBufferSkipClassT branch
// suppresses the §7 check for class-T owning spans. Guards against a
// false-reject regression on the class-T owning-span path.
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           class_t_span_accepts_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class K end K; "
        "class C "
        "  public static method T(sp: {TYPE}<K>, n: K?): int "
        "    return sp.IndexOf(n); "  // K? into class-T span — admissible (T?)
        "  end T; "
        "end C;";
    for (const char* type : {"Span", "ReadOnlySpan"}) {
        std::string src = tmpl;
        src.replace(src.find("{TYPE}"), 6, type);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    }
}

// #2600 / §4.15: the class-T skip is scoped to Span/ReadOnlySpan
// ONLY. ArraySpan<T>/ReadOnlyArraySpan<T> declare `value: T` for ALL T (Array
// slots do not wrap class T in T?), so a class-T ArraySpan<K>.IndexOf(K?)
// MUST reject — confirming the skip does not leak onto the ArraySpan family.
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           class_t_arrayspan_rejects_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class K end K; "
        "class C "
        "  public static method T(sp: {TYPE}<K>, n: K?): int "
        "    return sp.IndexOf(n); "  // K? into ArraySpan<K> — REJECT (value: T)
        "  end T; "
        "end C;";
    for (const char* type : {"ArraySpan", "ReadOnlyArraySpan"}) {
        std::string src = tmpl;
        src.replace(src.find("{TYPE}"), 6, type);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        std::string meth = std::string(type) + std::string("<T>.IndexOf");
        BEZEL_EXPECT_TRUE(hasError(r, meth.c_str()));
    }
}

// #2600 / §4.14: a NARROWED class-T `K?` on an ArraySpan<K> compiles (the §7.4
// narrowing exception lives inside checkAssignable, reused unchanged). Guards
// against the class-T-ArraySpan reject being over-broad.
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           class_t_arrayspan_accepts_narrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class K end K; "
        "class C "
        "  public static method T(sp: ArraySpan<K>): int "
        "    var n: K? := new K(); "  // K? — narrowed below
        "    if n = null then return -1; end; "
        "    return sp.IndexOf(n); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2600 regression guard: the fix extends #2582's checkBuiltinCollectionCallArgs
// gate, so the #2581 ReadOnlyStringSpan domain and the #2582 heap-collection
// domain must continue rejecting unchanged (no spillover false-accept from the
// new span-buffer branch's early-return path).
BEZEL_TEST(sema_span_buffer_nullable_arg_2600,
           cross_domain_2581_2582_still_reject) {
    // #2581 ReadOnlyStringSpan.IndexOf(string?) still rejects.
    auto r1 = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    return sp.IndexOf(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r1, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r1, "ReadOnlyStringSpan.IndexOf"));
    // #2582 Array<int>.IndexOf(int?) still rejects.
    auto r2 = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var arr := new Array<int>(1); "
        "    arr[0] := 7; "
        "    return arr.IndexOf(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r2, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r2, "Array<T>.IndexOf"));
}
