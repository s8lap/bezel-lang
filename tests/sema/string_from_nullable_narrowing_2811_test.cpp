// Spec-derived regression test for issue #2811.
//
// §7.4 (v0.240, "Member access on nullable values") / §32.6 (FFI marshalling,
// v0.240): `string.From(c: CString)` (and the `WString` twin) consumes a
// NON-nullable `CString`/`WString` parameter (stdlib §9.6). Passing a
// NULLABLE `CString?`/`WString?` without first narrowing it to non-null is a
// §7.4 violation — the same use-requires-narrowing discipline as member
// access on a nullable reference (`c.Length`, `c.Method()`, `c[i]`). Without
// this gate the call was silently accepted and the un-narrowed null reached
// the conversion at runtime; the #2734 runtime entry points are
// NULL-guarded and turned NULL into a `FormatException` — the wrong exception
// for a null reference (§31.4 / §7.4 say `NullReferenceException`), and the
// whole situation should have been a compile-time rejection.
//
// The pre-#2734 form had the same omission; before #2734 the whole call was
// uncompilable (TypeRef head name mismatch), so the hole never surfaced.
// #2734 widened the overload check to recognize the parameter-side TypeRef
// shape, opening the gate to nullable arguments for the first time.
//
// THE FIX (§7.4 narrowing consultation): mirror the resolveMemberAccess
// gate at `src/sema/ExprTypecheck_Calls.cpp` line ~13591 — when the argument
// is an FFI builtin CString/WString with `nullable == true`, consult the
// flow-narrowing side-sets (`nullNarrowed_` / `narrowings_`) via
// `isKnownNonNull` on the simple-var symbol (LocalSymbol or ParameterSymbol
// resolved through `lookupName`). Admit when the value is provably non-null
// at this call site; reject otherwise.
//
// This is a REJECTION fix, so each negative case asserts on diagnostic TEXT
// (not the coarse `hasErrors()` alone — a cascading second error from a
// different gate would not be distinguishable from a §7.4 violation). The
// positive cases guard against false rejection of the §7.4-narrowed form
// (the trap a naive `!at->nullable` conjunct would have triggered) and of
// the non-nullable-baseline shapes.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

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

// #2811 §7.4 / §32.6: the diagnostic fragment that names the §7.4
// use-requires-narrowing discipline on the `String.From` argument.
constexpr const char* kNarrowingDiag =
    "`String.From` argument is a nullable";

}  // namespace

// ---------------------------------------------------------------------------
// T1 (rejection, the issue's exact repro): `using var d := strdup(c)` makes
// `d : CString?`. Passing `d` to `string.From(d)` without narrowing was
// silently accepted pre-#2811, and ran with a wrong-exception runtime
// fallback (FormatException from the #2734 null guard, instead of the
// §31.4/§7.4 NullReferenceException). After #2811 the call is rejected
// at compile time with the §7.4 "narrow it to non-null first" diagnostic.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_string_from_nullable_narrowing_2811,
           repro_unnarrowed_cstring_from_rejected_sec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "extern \"c\" "
        "  public method strdup(s: CString): owned CString?; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var c := CString.From(\"abc\"); "
        "    using var d := strdup(c); "
        "    var s := string.From(d); "
        "    return s.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kNarrowingDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "§7.4"));
    BEZEL_EXPECT_TRUE(hasError(r, "§32.6"));
}

// ---------------------------------------------------------------------------
// T2 (ACCEPT control, §7.4 null-narrowing, the issue's "narrowed-inside-if"
// case): `if d <> null then string.From(d); end;` — the §7.4-narrowed shape.
// The fix MUST NOT over-reject this; the narrowing-consultation path
// (`isKnownNonNull` on `d`'s LocalSymbol, which `narrowFromNullTest` put in
// `nullNarrowed_` for the controlled block) is what admits it. A naive
// `!at->nullable` conjunct — the #2735 lane's "decline-with-reasons" —
// would have falsely rejected this case; the rejection fix is precisely
// the larger change the issue describes.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_string_from_nullable_narrowing_2811,
           narrowed_inside_if_cstring_from_accepted_control_sec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "extern \"c\" "
        "  public method strdup(s: CString): owned CString?; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var c := CString.From(\"abc\"); "
        "    using var d := strdup(c); "
        "    if d <> null then "
        "      var s := string.From(d); "
        "      return s.Length; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// T3 (ACCEPT control, non-nullable baseline): `using var c :=
// CString.From("abc")` makes `c : CString` (non-nullable); `string.From(c)`
// is the originally-accepted #2734 path (the issue's repro minus the
// `strdup`). The fix MUST NOT over-reject this; the
// `if (!at->nullable) ok = true;` short-circuit is what admits it.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_string_from_nullable_narrowing_2811,
           plain_nonnullable_cstring_from_accepted_control_sec_32_6) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var c := CString.From(\"abc\"); "
        "    var s := string.From(c); "
        "    return s.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// T4 (ACCEPT control, parameter shape from the #2734 collateral fix 4):
// `string.From(c)` where `c: CString` (a parameter) — the null-declaration
// TypeRef shape that #2734 widened. The fix MUST NOT regress this; the
// same `if (!at->nullable) ok = true;` path is what admits it, since
// `c: CString` (no `?`) has `at->nullable == false`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_string_from_nullable_narrowing_2811,
           parameter_cstring_from_accepted_control_stdlib_9_6) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method read(c: CString): int "
        "    var s := string.From(c); "
        "    return s.Length; "
        "  end read; "
        "  public static method Main(): int "
        "    using var c := CString.From(\"abc\"); "
        "    return read(c); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// T5 (rejection, the WString twin): the #2811 omission generalizes to
// `string.From(w)` where `w: WString?` — the same head-name `isFfiBuiltin`
// predicate covers `CString` AND `WString` (the #1145 widening pass names
// both). Pin the WString rejection to confirm the fix sweeps both.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_string_from_nullable_narrowing_2811,
           unnarrowed_wstring_from_rejected_sec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "extern \"c\" "
        "  public method wcsdup(s: WString): owned WString?; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var w0 := WString.From(\"abc\"); "
        "    using var w := wcsdup(w0); "
        "    var s := string.From(w); "
        "    return s.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kNarrowingDiag));
}

// ---------------------------------------------------------------------------
// T6 (ACCEPT control, narrowed WString inside if): the WString twin of T2.
// `if w <> null then string.From(w); end;` is admitted by the same
// narrowing-consultation path.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_string_from_nullable_narrowing_2811,
           narrowed_inside_if_wstring_from_accepted_control_sec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "extern \"c\" "
        "  public method wcsdup(s: WString): owned WString?; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var w0 := WString.From(\"abc\"); "
        "    using var w := wcsdup(w0); "
        "    if w <> null then "
        "      var s := string.From(w); "
        "      return s.Length; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// T7 (rejection, `c!` is the SHORT-CIRCUIT path, not the narrowing path):
// `c!` rewrites the operand's exprType to its non-nullable form via
// `visitExpr`'s PostfixBangExpr arm (per the throw-#3076 throw-side fix's
// observation), so `string.From(c!)` over `c: CString?` is admitted by
// the `if (!at->nullable) ok = true;` short-circuit (NOT by the narrowing
// path). An over-aggressive narrowing-only fix — one that drops the
// short-circuit and requires a `nullNarrowed_` fact even for `c!` —
// would FAIL this test, so the pin is part of the contract.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_string_from_nullable_narrowing_2811,
           postfix_bang_cstring_from_accepted_control_sec_7_8) {
    auto r = parseSemaAll(
        "module M; "
        "extern \"c\" "
        "  public method strdup(s: CString): owned CString?; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var c0 := CString.From(\"abc\"); "
        "    using var c := strdup(c0); "
        "    var s := string.From(c!); "
        "    return s.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
