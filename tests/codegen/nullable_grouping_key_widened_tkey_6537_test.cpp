// fix #6537 (spec §7.5 v0.338 / stdlib §5 v0.338): `g?.Key` on an
// `IGrouping<TKey, T>` SILENTLY DROPPED for any `TKey` whose lowered LLVM shape
// was neither a pointer nor an integer of at most 32 bits — `float`, `double`,
// `long`, `ulong`, `decimal`. The store was dropped, the target kept its
// previous value, and the `??` fallback fired as though the access had returned
// null. The program COMPILED, RAN and EXITED 0 PRINTING THE WRONG ANSWER, with
// the `internal:` #2961 backstop the only signal — and that is a Warning, not
// an Error, so nothing rejected it.
//
// THE RULE. spec §7.5 (v0.338), verbatim: "`x?.M` returns null if `x` is null;
// otherwise it performs the access. The result type is always nullable." The
// section restricts the RECEIVER's nullability; it names NO restriction on the
// member's result type. stdlib §5 (v0.338) declares exactly one own member on
// `IGrouping<TKey, T>` — `readonly property Key: TKey` — for EVERY `TKey`, and
// stdlib §5's `GroupBy` takes `TKey` from the selector's return type, so a
// `double` or `long` selector is an ordinary instantiation, not an exotic one.
// So `g?.Key` is `TKey?` for all of them and MUST perform the access: a
// nullable VALUE type (§7.2's `{i8 hasValue; T}` tagged struct) is a
// first-class nullable representation for `float`/`double`/`long`/`decimal`
// exactly as it is for `int` — this is not a new representation, it is the one
// `lowerTypeRef` already gives every `TKey?`.
//
// WHERE IT WAS LOST (codegen, the ONE site). The `?.`-`Key` arm in
// LlvmBackend_Exprs.cpp gated on the LLVM SHAPE of `TKey` —
// `isIntegerTy() || isPointerTy()`, plus `getIntegerBitWidth() > 32` — and
// `return nullptr` otherwise. Upstream, nullptr is #2961's "the RHS produced no
// value": the store is dropped and the previous value survives. The `.` form
// reaches the SAME `bz_grouping_key` intrinsic through the MemberAccessExpr arm
// with NO shape gate, which is why `dotfloatsum=4` beside `floatsum=0` on the
// issue's own repro. The fix replaces the shape test with the compiler's own
// nullable-shape predicate `isNullableValueTy` over a nullable copy of `TKey`
// — the same predicate `lowerTypeRef` consults to give a `TKey?` its
// representation, so the value the arm builds is bit-for-bit what every `TKey?`
// consumer already expects. A pointer-typed `TKey` keeps the bare-nullable-
// pointer shape (a null ptr IS the null value), which is the shape the #6512
// review measured as required by `(g?.Key ?? "?").Length`.
//
// THE ORACLE, and why `exitCode` alone is not one. The pre-fix program exits 0
// with the wrong answer, so every value row here asserts the PRINTED value
// (`JitResult::stdoutText`, captured by the AOT subprocess) or a `??`-derived
// exit code, and each row carries a same-shape `.`-form CONTROL whose expected
// value is stated with it — a fix that merely produced SOME value (zero, or the
// `??` default) fails the control. Row R5 pins the widened `TKey` against the
// `.`-form answer IN THE SAME PROGRAM, which is the strongest form: the two
// spellings must agree.
//
// ON DIAGNOSTIC ABSENCE. The natural extra assertion — "the #2961 warning is
// gone" — is VACUOUS on this oracle and is deliberately NOT made: `runAot`
// reports Warnings through the forked compile's stderr, and
// `JitResult::diagnostics` is empty by construction whenever `r.ran` is true
// (the known #3138 trap; `scripts/check_vacuous_tests.py` flags the pattern).
// An `EXPECT_FALSE(hasWarning(r))` here would be a tautology that passes on the
// BROKEN build for the wrong reason. The diagnostic-absence oracle for this
// defect lives on the lit track instead, in
// `tests/lit/nullcond_grouping_key_no_2961_6537.bzl`, which greps `bezelc -c`
// stderr — a channel that IS populated on an accept.

#include <string>

#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"           // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// ---------------------------------------------------------------------------
// The key-type matrix. Each program sums `g?.Key` over `GroupBy(identity)` —
// so every group contributes ITS OWN KEY once — and prints the sum on stdout.
//
// Grouping is by IDENTITY, so each distinct key is its own group and the sum
// of the `?.` keys must equal the sum of the `.` keys, which for an identity
// selector over a duplicate-free list is the sum of the LIST. Both are stated
// per row, and the `.`-form is computed IN THE SAME PROGRAM by a second loop
// (row R5's contract, applied to every row) so a wrong answer on either
// spelling is visible side by side.
// ---------------------------------------------------------------------------

// R1 — `double` key (the issue's `floatsum` row, verbatim shape).
// MEASURED pre-fix: `floatsum=0` (drop, fallback taken), `dotfloatsum=4`.
// Expected post-fix: `nc=4` and `dot=4`.
const char* kDoubleKey =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<double> := [1.5, 2.5]; "
    "    var s := 0.0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + (g?.Key ?? 0.0); "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    var d := 0.0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      d := d + g.Key; "
    "    end; "
    "    Console.WriteLine($\"dot={d}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R2 — `long` key (the issue's `longsum` row, verbatim shape).
// MEASURED pre-fix: `longsum=0`. Expected post-fix: `nc=6` and `dot=6`.
const char* kLongKey =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<long> := [1L, 2L, 3L]; "
    "    var s := 0L; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + (g?.Key ?? 0L); "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    var d := 0L; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      d := d + g.Key; "
    "    end; "
    "    Console.WriteLine($\"dot={d}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R3 — `float` key. Pre-fix measured: `nc=0`, `dot=4`. Post-fix: 4 and 4.
const char* kFloatKey =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<float> := [1.5f, 2.5f]; "
    "    var s := 0.0f; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + (g?.Key ?? 0.0f); "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    var d := 0.0f; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      d := d + g.Key; "
    "    end; "
    "    Console.WriteLine($\"dot={d}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R4 — `ulong` key (the unsigned 64-bit axis; a DISTINCT lowered shape from
// `long`, so it is its own row rather than a claim about both).
// Pre-fix measured: `nc=0`, `dot=6`. Post-fix: 6 and 6.
const char* kULongKey =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<ulong> := [1UL, 2UL, 3UL]; "
    "    var s := 0UL; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + (g?.Key ?? 0UL); "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    var d := 0UL; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      d := d + g.Key; "
    "    end; "
    "    Console.WriteLine($\"dot={d}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R5 — `decimal` key: a 16-byte STRUCT payload (§4.4, #521), so the widened
// tagged struct `{i8 hasValue; %bz.decimal}` is an AGGREGATE payload rather than
// a scalar — the shape that would break if the fix had used a raw
// `{i8, lowerTypeRef(TKey)}` instead of the shared `nullableValueStructTy`.
// Pre-fix measured: `nc=0`, `dot=4`. Post-fix: `4` and `4` — the decimal
// interpolation renders `4.0` as `4`, which is why the EXPECTATION below is
// `nc=4\ndot=4\n` and not `nc=4.0\n` (MEASURED, not assumed).
const char* kDecimalKey =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<decimal> := [1.5m, 2.5m]; "
    "    var s := 0.0m; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + (g?.Key ?? 0.0m); "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    var d := 0.0m; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      d := d + g.Key; "
    "    end; "
    "    Console.WriteLine($\"dot={d}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// CONTROLS — the key-type shapes that ALREADY worked before the fix. These are
// the regression fence: the fix replaces the LLVM-shape gate with
// `isNullableValueTy`, so a predicate that is too broad could admit a shape the
// old gate declined on purpose (a `string` key must stay a BARE nullable
// pointer — wrapping it in the tagged struct is what made #6512's
// `(g?.Key ?? "?").Length` GEP into the struct), and a predicate that were too
// narrow would break these. Each row's expected value is stated, and each was
// MEASURED identical pre- and post-fix.
// ---------------------------------------------------------------------------

// R6 — `int` key, the ≤32-bit-integer shape that already lowered. Sum 1+2+3=6.
const char* kIntKeyControl =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<int> := [1, 2, 3]; "
    "    var s := 0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + (g?.Key ?? 0); "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R7 — `byte` key: an 8-bit integer, the OTHER end of the old ≤32-bit gate.
// Sum 1+2+3=6.
const char* kByteKeyControl =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<byte> := [1, 2, 3]; "
    "    var s := 0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + int(g?.Key ?? 0); "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R8 — `short` key: a 16-bit integer. Sum 1+2+3=6.
const char* kShortKeyControl =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<short> := [1, 2, 3]; "
    "    var s := 0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + int(g?.Key ?? 0); "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R9 — `char` key: an i32-LOWERING value type (§7.1, #849), so it is a value
// TKey (tagged struct) and NOT a reference. 'a'+'b'+'c' = 97+98+99 = 294, which
// exceeds the 8-bit exit channel, so it is on stdout.
const char* kCharKeyControl =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<char> := ['a', 'b', 'c']; "
    "    var s := 0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + int(g?.Key ?? 'z'); "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R10 — `string` key: the REFERENCE shape. The value must remain a BARE
// nullable pointer, so `(g?.Key ?? "?").Length` must work — the exact shape
// #6512's review measured as broken when the key was wrapped in the tagged
// struct instead ("GEP base pointer is not a vector or a vector of pointers").
// "ab".Length=2 + "cde".Length=3 = 5.
const char* kStringKeyReference =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<string> := [\"ab\", \"cde\"]; "
    "    var s := 0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + (g?.Key ?? \"?\").Length; "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R11 — a `?.` ENUM key: `Col?` lowers to `{i8 hasValue; i32}` (the enum's
// underlying integer, §10.3 / #740), a shape the old gate's plain
// `isIntegerTy()` test happened to admit. Exactly one group has `Col.Red`.
// This row pins that the widened predicate did not disturb the enum payload.
const char* kEnumKey =
    "module M; public enum Col Red, Green, Blue; end Col; class C "
    "  public static method Main(): int "
    "    var xs: Array<Col> := [Col.Red, Col.Green, Col.Blue]; "
    "    var n := 0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      var k := g?.Key; "
    "      if k = Col.Red then n := n + 1; end; "
    "    end; "
    "    Console.WriteLine($\"nc={n}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// R12 — the ACCESS IS PERFORMED, NOT SHORT-CIRCUITED, at a widened `TKey`.
// §7.5 short-circuits on a NULL RECEIVER; `g` is a non-nullable `IGrouping`, so
// the check `k <> null` must hold for every group and the VALUE must be folded
// in. Per #6512's own lesson a bare `seen + 1` behind the narrowing check
// asserts nothing (the degraded slot compared non-equal to null and the branch
// still ran, with a zero value) — so the VALUE is folded in, and the `?? -1`
// sentinel distinguishes "key read" (4) from "dropped" (0) and from "took the
// `??` default" (-2).
// MEASURED pre-fix: `seen=0`. Expected post-fix: `seen=4`.
// ---------------------------------------------------------------------------
const char* kDoubleKeyIsPerformed =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<double> := [1.5, 2.5]; "
    "    var seen := 0.0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      var k := g?.Key; "
    "      if k <> null then seen := seen + (k ?? -1.0); end; "
    "    end; "
    "    Console.WriteLine($\"nc={seen}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R13 — the widened key in a LOCAL-DECLARATION initializer. #6537's site is the
// `:=` arm, whose #2961 degrade reads "the RHS produced no value"; the
// local-declaration position has its OWN #2434 backstop with a DIFFERENT
// message and a different degrade (a zero-initialized slot rather than a
// retained previous value). Both are silent at exit 0, so both are worth
// pinning: `var k: double? := g?.Key;` then `(k ?? -1.0)` must read 4.0.
// MEASURED pre-fix: `nc=0`. Expected post-fix: `nc=4`.
const char* kDoubleKeyLocalDecl =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<double> := [1.5, 2.5]; "
    "    var s := 0.0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      var k: double? := g?.Key; "
    "      s := s + (k ?? -1.0); "
    "    end; "
    "    Console.WriteLine($\"nc={s}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// R14 — the TYPE the widened form produces is `double?`, not `double`: §7.11
// (v0.267) forbids arithmetic on a nullable value type, so `s + g?.Key` must be
// REJECTED. This is the diagnostic that SURVIVES the fix (the `internal:` #2961
// backstop is the needle that dies), so it is the one to pin. Same shape for a
// `long` key, whose `TKey?` is the 64-bit payload.
const char* kDoubleKeyNotLiftedOverAddition =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<double> := [1.5, 2.5]; "
    "    var s := 0.0; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + g?.Key; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kLongKeyNotLiftedOverAddition =
    "module M; class C "
    "  public static method Main(): int "
    "    var xs: Array<long> := [1L, 2L]; "
    "    var s := 0L; "
    "    for var g in xs.GroupBy(lambda(x) => x) do "
    "      s := s + g?.Key; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kNullableOperandNeedle =
    "operator `+` is not defined on a nullable value type";

// Run and return the captured stdout; `""` if the program did not run.
std::string runOut(const char* src) {
    JitResult r = runAotChecked(src);
    if (!r.ran) return std::string{};
    return r.stdoutText;
}

}  // namespace

// ===========================================================================
// THE WIDENED TKey AXIS — every row was MEASURED 0 (or the `??` default) on
// 26ca3bd9, i.e. wrong at exit 0. Each asserts the PRINTED sum and the `.`-form
// control from the SAME program.
// ===========================================================================

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           double_key_agrees_with_dot_form_spec_7_5) {
    JitResult r = runAotChecked(kDoubleKey);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("nc=4\ndot=4\n"));
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           long_key_agrees_with_dot_form_spec_7_5) {
    JitResult r = runAotChecked(kLongKey);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("nc=6\ndot=6\n"));
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           float_key_agrees_with_dot_form_spec_7_5) {
    JitResult r = runAotChecked(kFloatKey);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("nc=4\ndot=4\n"));
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           ulong_key_agrees_with_dot_form_spec_7_5) {
    JitResult r = runAotChecked(kULongKey);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("nc=6\ndot=6\n"));
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           decimal_aggregate_payload_agrees_with_dot_form_spec_7_5) {
    JitResult r = runAotChecked(kDecimalKey);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("nc=4\ndot=4\n"));
}

// ===========================================================================
// CONTROLS — the shapes that already worked. Expected values MEASURED
// identical on 26ca3bd9 and on the fix; a change here is a REGRESSION the fix
// introduced, not a row this issue is claiming.
// ===========================================================================

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           int32_key_control_still_reads_the_key_spec_7_5) {
    BEZEL_EXPECT_EQ(runOut(kIntKeyControl), std::string("nc=6\n"));
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           byte_key_control_still_reads_the_key_spec_7_5) {
    BEZEL_EXPECT_EQ(runOut(kByteKeyControl), std::string("nc=6\n"));
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           short_key_control_still_reads_the_key_spec_7_5) {
    BEZEL_EXPECT_EQ(runOut(kShortKeyControl), std::string("nc=6\n"));
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           char_key_control_still_reads_the_key_spec_7_5) {
    BEZEL_EXPECT_EQ(runOut(kCharKeyControl), std::string("nc=294\n"));
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           string_key_stays_a_bare_nullable_reference_spec_7_5) {
    BEZEL_EXPECT_EQ(runOut(kStringKeyReference), std::string("nc=5\n"));
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           enum_key_payload_unchanged_spec_7_5) {
    BEZEL_EXPECT_EQ(runOut(kEnumKey), std::string("nc=1\n"));
}

// ===========================================================================
// THE ACCESS IS PERFORMED AT A WIDENED TKey — §7.5 short-circuits on a null
// RECEIVER, and the receiver here is non-nullable, so every group must
// contribute its key. The `?? -1.0` sentinel separates the three outcomes the
// issue could not tell apart: read (4.0), dropped (0.0), default (-2.0).
// ===========================================================================

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           double_key_is_performed_not_short_circuited_spec_7_5) {
    BEZEL_EXPECT_EQ(runOut(kDoubleKeyIsPerformed), std::string("nc=4\n"));
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           double_key_local_decl_initializer_lowers_spec_7_5) {
    BEZEL_EXPECT_EQ(runOut(kDoubleKeyLocalDecl), std::string("nc=4\n"));
}

// ===========================================================================
// THE DIAGNOSTIC THAT SURVIVES — the widened result is `TKey?` (§7.5's
// "always nullable"), so §7.11 (v0.267) still rejects arithmetic on it. This is
// the needle that does NOT die with the fix; the `internal:` #2961 backstop is
// the one that does, so it must not be what is asserted here.
// ===========================================================================

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           double_key_is_nullable_double_spec_7_5_7_11) {
    JitResult r = runAot(kDoubleKeyNotLiftedOverAddition);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kNullableOperandNeedle) != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

BEZEL_TEST(nullable_grouping_key_widened_tkey_6537,
           long_key_is_nullable_long_spec_7_5_7_11) {
    JitResult r = runAot(kLongKeyNotLiftedOverAddition);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kNullableOperandNeedle) != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}