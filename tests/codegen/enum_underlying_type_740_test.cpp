// Spec-derived tests for fix #740 — long/ulong (and every non-int) enum
// underlying type is represented at its declared width, not truncated to i32.
//
// Authority: §10.3 "The default underlying type is `int`. A different integer
// type may be specified with a colon ... must be one of `byte`, `sbyte`,
// `short`, `ushort`, `int`, `uint`, `long`, `ulong`. All declared values must
// fit in the underlying type's range." §5.4 (enum/int casts), §8.5 (ToString).
//
// Pre-fix the enum representation was hardcoded to i32 ("CG44"): a `: long`
// constant such as 9_000_000_000 truncated to its low 32 bits (410065408)
// across value materialization, casts, comparison, `case`, the enum method
// `this`, nullable enums, and serialization. These tests pin each surface for
// the headline `long` case and exercise every underlying type.

#include "support/aot_result_asserts.hpp"   // fix #3895: failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── §10.3: headline — a `: long` constant round-trips without truncation ─────
BEZEL_TEST(enum_underlying_740, long_constant_round_trips) {
    // per §10.3 / §5.4 — long(BigEnum.Big) must be 9000000000, not 410065408.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum BigEnum: long Big := 9000000000L; end BigEnum; "
        "class C public static method Main(): int "
        "  var b: long := long(BigEnum.Big); "
        "  return (if b = 9000000000L then 0 else 1); "
        "end Main; end C;"), 0);
}

// §10.3: a value above 2^31 used as a `case` scrutinee matches at full width.
BEZEL_TEST(enum_underlying_740, long_case_dispatch_full_width) {
    // per §10.3 — Big.B (9e9) and Big.C (3) are distinguished at i64 width.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Big: long A := 1L, B := 9000000000L, C := 3L; end Big; "
        "class C public static method Main(): int "
        "  var x: Big := Big.B; "
        "  return (case x of Big.A => 1, Big.B => 2, Big.C => 3); "
        "end Main; end C;"), 2);
}

// §10.3: equality on a `: long` enum compares at full width.
BEZEL_TEST(enum_underlying_740, long_equality_full_width) {
    // per §10.3
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Big: long A := 1L, B := 9000000000L; end Big; "
        "class C public static method Main(): int "
        "  var x: Big := Big.B; "
        "  if x <> Big.B then return 1; end; "
        "  if x = Big.A then return 2; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §5.4: the integer→enum cast `Big(i)` keeps the full underlying width.
BEZEL_TEST(enum_underlying_740, int_to_long_enum_cast) {
    // per §5.4 — Big(9000000000L) must equal Big.B.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Big: long A := 1L, B := 9000000000L; end Big; "
        "class C public static method Main(): int "
        "  var b: Big := Big(9000000000L); "
        "  return (if b = Big.B then 0 else 1); "
        "end Main; end C;"), 0);
}

// §10.3: a `byte`-backed value ≥ 128 is unsigned — int() zero-extends it.
BEZEL_TEST(enum_underlying_740, byte_unsigned_zero_extends) {
    // per §10.3 / §5.4 — int(Color.Hi) is 200, not -56.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color: byte Lo := 0, Hi := 200; end Color; "
        "class C public static method Main(): int "
        "  return int(Color.Hi); "
        "end Main; end C;"), 200);
}

// §10.3: an `sbyte`-backed negative value sign-extends through int().
BEZEL_TEST(enum_underlying_740, sbyte_negative_sign_extends) {
    // per §10.3 / §5.4
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum E: sbyte Neg := -100, Pos := 100; end E; "
        "class C public static method Main(): int "
        "  return (if int(E.Neg) = -100 then 0 else 1); "
        "end Main; end C;"), 0);
}

// §10.3: `short` / `ushort` widths round-trip.
BEZEL_TEST(enum_underlying_740, short_and_ushort_round_trip) {
    // per §10.3
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum ES: short A := -30000, B := 30000; end ES; "
        "enum EUS: ushort A := 0, B := 60000; end EUS; "
        "class C public static method Main(): int "
        "  if int(ES.A) <> -30000 then return 1; end; "
        "  if int(EUS.B) <> 60000 then return 2; end; "
        "  var s: ES := ES(30000); "
        "  if s <> ES.B then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §10.3: `uint` / `ulong` values above the signed max widen unsigned.
BEZEL_TEST(enum_underlying_740, uint_and_ulong_above_signed_max) {
    // per §10.3 / §5.4
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum EUI: uint  A := 0, B := 4000000000U;  end EUI; "
        "enum EUL: ulong A := 0, B := 18000000000UL; end EUL; "
        "class C public static method Main(): int "
        "  if long(EUI.B) <> 4000000000L then return 1; end; "
        "  if long(EUL.B) <> 18000000000L then return 2; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §10.3: an enum-keyed dictionary keys by the full underlying value.
BEZEL_TEST(enum_underlying_740, long_enum_keyed_dictionary) {
    // per §10.3 / §4.8
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "enum Big: long A := 1L, B := 9000000000L; end Big; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Big, int>(); "
        "  d[Big.B] := 42; d[Big.A] := 7; "
        "  return (if d[Big.B] = 42 then 0 else 1); "
        "end Main; end C;"), 0);
}

// §7.2 / §10.3: a nullable `: long` enum stores the full value and a null tag.
BEZEL_TEST(enum_underlying_740, nullable_long_enum) {
    // per §7.2 / §10.3
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Big: long A := 1L, B := 9000000000L; end Big; "
        "class C public static method Main(): int "
        "  var n: Big? := Big.B; "
        "  if n = null then return 1; end; "
        "  if n! <> Big.B then return 2; end; "
        "  var m: Big? := null; "
        "  if m <> null then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §10.3 / §10.5: an enum instance method receives `this` at the underlying width.
BEZEL_TEST(enum_underlying_740, enum_method_this_full_width) {
    // per §10.3 — long(this) inside Doubled() must be 9e9, not the truncation.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Big: long "
        "  A := 1L, B := 9000000000L; "
        "  public method Doubled(): long return long(this) * 2L; end Doubled; "
        "end Big; "
        "class C public static method Main(): int "
        "  return (if Big.B.Doubled() = 18000000000L then 0 else 1); "
        "end Main; end C;"), 0);
}

// §9.8 / §10.3: a serializable `: long` enum round-trips by name AND keeps its
// full underlying value on the deserialized side.
BEZEL_TEST(enum_underlying_740, long_enum_serialization_round_trip) {
    // per §9.8 (enums serialize by member name) / §10.3
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "enum Big: long, ISerializable, IDeserializable "
        "  A := 1L, B := 9000000000L, C := 3L; end Big; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  Big.B.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := Big.Deserialize(d); "
        "  if v <> Big.B then return 1; end; "
        "  if long(v) <> 9000000000L then return 2; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §8.5: enum default ToString is the type's qualified name (width-independent,
// but pinned here to guard against a value-reading regression).
BEZEL_TEST(enum_underlying_740, long_enum_tostring_is_type_name) {
    // per §8.5
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Big: long A := 1L, B := 9000000000L; end Big; "
        "class C public static method Main(): int "
        "  var x: Big := Big.B; "
        // #2934 (§8.5, v0.252): was "Big" — the enum default is the
        // module-path-qualified type name, independent of §10.3's
        // underlying type.
        "  return (if x.ToString() = \"M.Big\" then 0 else 1); "
        "end Main; end C;"), 0);
}

// Regression guard: a default (int) enum is unchanged — still i32-represented.
BEZEL_TEST(enum_underlying_740, default_int_enum_unchanged) {
    // per §10.3 — default underlying type is int.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Dir North := 0, South := 1, East := 2, West := 3; end Dir; "
        "class C public static method Main(): int "
        "  var d: Dir := Dir.East; "
        "  if int(d) <> 2 then return 1; end; "
        "  return (case d of Dir.North => 10, Dir.East => 0, _ => 9); "
        "end Main; end C;"), 0);
}

// §10.2/§10.3 (#740): an enum member value is "a compile-time constant of the
// underlying type", so an unsuffixed integer literal that exceeds `int` must
// widen to the enum's `long`/`ulong`/`uint` underlying type — the same target-
// typing a typed local initializer gets (`var x: long := 5000000000;`, issue
// #184). Pre-fix, the literal was typed as `int` first and rejected ("does not
// fit in `int`; add a suffix"), forcing every long-enum member to carry an `L`.
BEZEL_TEST(enum_underlying_740, long_member_bare_literal_no_suffix) {
    // per §10.2 — `5000000000` (no `L`) is accepted and round-trips at i64.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Big: long A := 5000000000, B := 6000000000; end Big; "
        "class C public static method Main(): int "
        "  var x: Big := Big.B; "
        "  return (if long(x) = 6000000000L then 0 else 1); "
        "end Main; end C;"), 0);
}

// §10.2/§10.3 (#740): a `uint` member literal above int.MaxValue, no suffix.
BEZEL_TEST(enum_underlying_740, uint_member_bare_literal_no_suffix) {
    // per §10.2 — `4000000000` (> 2^31, no `U`) is accepted under a `: uint` enum.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum UI: uint A := 4000000000; end UI; "
        "class C public static method Main(): int "
        "  return (if long(UI.A) = 4000000000L then 0 else 1); "
        "end Main; end C;"), 0);
}

// §10.2/§10.3 (#740): a `ulong` member literal above int.MaxValue, no suffix —
// exercises the ULong widening hint and round-trips. (The high `ulong` range,
// ≥ 2^63, is covered by the #749 tests below.)
BEZEL_TEST(enum_underlying_740, ulong_member_bare_literal_no_suffix) {
    // per §10.2 — `5000000000` (> 2^31, < 2^63, no `UL`) accepted under `: ulong`.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum U: ulong A := 5000000000; end U; "
        "class C public static method Main(): int "
        "  return (if ulong(U.A) = 5000000000UL then 0 else 1); "
        "end Main; end C;"), 0);
}

// §10.3 (#740): the widening is bounded by the underlying type — an `int`-backed
// enum (default) still rejects an over-`int` member literal (no widening hint).
BEZEL_TEST(enum_underlying_740, int_member_over_int_still_rejected) {
    // per §10.3 — default underlying type is `int`; 5e9 does not fit.
    auto r = runAot(
        "module M; "
        "enum E: int A := 5000000000; end E; "
        "class C public static method Main(): int return 0; end Main; end C;");
    // fix #3895 (issue #3892): distinguishes a genuine §10.3 rejection from a
    // runtime crash/signal, which a bare `hasErrors()`/`!ran` pair cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// §10.3 (#740): a value beyond the underlying type's own range is still rejected
// even with the widening hint (a `: long` member exceeding long.MaxValue).
BEZEL_TEST(enum_underlying_740, long_member_over_long_still_rejected) {
    // per §10.3 — "all declared values must fit in the underlying type's range".
    auto r = runAot(
        "module M; "
        "enum Big: long A := 99999999999999999999999; end Big; "
        "class C public static method Main(): int return 0; end Main; end C;");
    // fix #3895 (issue #3892): distinguishes a genuine §10.3 rejection from a
    // runtime crash/signal, which a bare `hasErrors()`/`!ran` pair cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// §10.2/§10.3 (#749): a `ulong` member value in [2^63, 2^64) round-trips. The
// member-value fold (sema::foldIntConst) parsed literals with signed `stoll`,
// which threw for any value above long.MaxValue (2^63); the enum value then
// silently fell back to the auto-increment running counter (0 for the first
// member). The fold now parses the high half via `stoull` and preserves the
// bit pattern, which the i64 materialization reads back as the right unsigned
// value. Plain `ulong` locals were always fine — the defect was enum-specific.
BEZEL_TEST(enum_underlying_749, ulong_member_above_long_max_round_trips) {
    // per §10.2 — 1.8e19 (> 2^63, < 2^64) survives, was 0.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum U: ulong A := 18000000000000000000UL; end U; "
        "class C public static method Main(): int "
        "  return (if ulong(U.A) = 18000000000000000000UL then 0 else 1); "
        "end Main; end C;"), 0);
}

// §10.2/§10.3 (#749): the exact 2^63 boundary (the first value `stoll` rejects)
// round-trips, both with and without an explicit `UL` suffix.
BEZEL_TEST(enum_underlying_749, ulong_member_at_2p63_round_trips) {
    // per §10.2 — 9223372036854775808 == 2^63.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum U: ulong A := 9223372036854775808, B := 9223372036854775808UL; end U; "
        "class C public static method Main(): int "
        "  if ulong(U.A) <> 9223372036854775808UL then return 1; end; "
        "  return (if ulong(U.B) = 9223372036854775808UL then 0 else 2); "
        "end Main; end C;"), 0);
}

// §10.2 (#749): auto-increment continues correctly from a >2^63 explicit value
// (the running counter advances on the preserved bit pattern: B == A + 1).
BEZEL_TEST(enum_underlying_749, ulong_auto_increment_above_2p63) {
    // per §10.2 — B := A + 1 where A == 2^63.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum U: ulong A := 9223372036854775808UL, B; end U; "
        "class C public static method Main(): int "
        "  return (if ulong(U.B) = 9223372036854775809UL then 0 else 1); "
        "end Main; end C;"), 0);
}

// §10.2 (#749 codegen counterpart): a `: long` enum member holding long.MinValue
// round-trips and compares equal to the long.MinValue literal. The enum fold
// (foldIntConst, stoull) was already correct, but the literal lowered through
// strtoll-saturation to INT64_MIN+1 — so `long(L.N) = -9223372036854775808L`
// was false even though the enum value's bits were right. Both sides now agree.
BEZEL_TEST(enum_underlying_749, long_min_value_enum_member_round_trips) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum L: long N := -9223372036854775808L; end L; "
        "class C public static method Main(): int "
        "  return (if long(L.N) = -9223372036854775808L then 0 else 1); "
        "end Main; end C;"), 0);
}

// §5 (#749 follow-up): an explicitly-suffixed literal whose magnitude overflows
// 64 bits is unrepresentable in any integer type and must be rejected. The
// unsuffixed path always did (via the `int` branch); the suffixed path did not,
// so `…UL` beyond ulong.MaxValue was silently accepted and miscompiled to 0.
BEZEL_TEST(enum_underlying_749, ulong_literal_over_64bit_rejected) {
    // per §5 — 1e23 (≫ 2^64-1) carries an explicit `UL` yet is unrepresentable.
    auto r = runAot(
        "module M; "
        "enum U: ulong A := 99999999999999999999999UL; end U; "
        "class C public static method Main(): int return 0; end Main; end C;");
    // fix #3895 (issue #3892): distinguishes a genuine §10.3 rejection from a
    // runtime crash/signal, which a bare `hasErrors()`/`!ran` pair cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// §5 (#749 follow-up): the boundary value ulong.MaxValue (2^64-1) is in range
// and must still be accepted — the rejection above triggers only on a true
// 64-bit overflow, not on the maximum representable value.
BEZEL_TEST(enum_underlying_749, ulong_max_value_accepted) {
    // per §5 / §10.2 — 18446744073709551615 == 2^64-1 == ulong.MaxValue.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum U: ulong Max := 18446744073709551615UL; end U; "
        "class C public static method Main(): int "
        "  return (if ulong(U.Max) = 18446744073709551615UL then 0 else 1); "
        "end Main; end C;"), 0);
}

// §5 (#749 follow-up) regression guard: long.MinValue (-2^63) must NOT be
// rejected. Its inner literal magnitude is 2^63 (a leading `-` is a UnaryExpr),
// which is exactly the value the over-64-bit check must continue to admit — the
// reject fires only when strtoull itself overflows (value > 2^64-1).
BEZEL_TEST(enum_underlying_749, long_min_value_literal_accepted) {
    // per §5 — -9223372036854775808 == -(2^63) == long.MinValue.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: long := -9223372036854775808L; "
        "  return (if x = -9223372036854775808L then 0 else 1); "
        "end Main; end C;"), 0);
}

// §8.4/§10.3 (#1189): a relational comparison (`<` `<=` `>` `>=`) of two
// unsigned-backed enum values must use the UNSIGNED integer predicate. The enum
// representation is its underlying integer (§10.3); with a `byte`-backed enum a
// value above the signed midpoint (200 = 0xC8) read as signed is -56, so a
// signed `icmp slt` made `Code.Lo (10) < Code.Hi (200)` evaluate FALSE. The
// detection helpers (isUnsignedBinOperand/isUnsignedTargetExpr) only recognised
// Primitive-kind operands; a Named-kind enum fell through to the signed path.
// §10.5 (v0.314/#5398): ordering ops on enum with no declared operator group
// are rejected. These tests verify that explicit casts to underlying type give
// correct results (the remedy the diagnostic recommends).
BEZEL_TEST(enum_underlying_1189, unsigned_enum_relational_compare) {
    // per §10.5 (v0.314/#5398): ordering ops on enum without a declared operator
    // group are a compile error. Use explicit casts to the underlying type instead.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Code: byte Lo := 10, Hi := 200; end Code; "
        "class C public static method Main(): int "
        "  if not (int(Code.Lo) < int(Code.Hi))  then return 1; end; "
        "  if not (int(Code.Lo) <= int(Code.Hi)) then return 2; end; "
        "  if not (int(Code.Hi) > int(Code.Lo))  then return 3; end; "
        "  if not (int(Code.Hi) >= int(Code.Lo)) then return 4; end; "
        "  if not (int(Code.Lo) <= int(Code.Lo)) then return 5; end; "
        "  if not (int(Code.Hi) >= int(Code.Hi)) then return 6; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §10.5 (v0.314/#5398): same for ushort-backed enums.
BEZEL_TEST(enum_underlying_1189, unsigned_enum_relational_compare_ushort_uint) {
    // per §10.5 (v0.314/#5398): ordering ops on enum without declared group are
    // rejected; use explicit casts to the underlying type.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum US: ushort Lo := 100, Hi := 40000; end US; "
        "class C public static method Main(): int "
        "  if not (int(US.Lo) < int(US.Hi)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §10.5 (v0.314/#5398): signed underlying type — same explicit-cast approach.
BEZEL_TEST(enum_underlying_1189, signed_enum_relational_compare_unaffected) {
    // per §10.5 (v0.314/#5398): ordering ops on enum without declared group are
    // rejected; use explicit casts to the underlying type.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum E: sbyte Neg := -100, Pos := 100; end E; "
        "class C public static method Main(): int "
        "  if not (int(E.Neg) < int(E.Pos))  then return 1; end; "
        "  if not (int(E.Pos) > int(E.Neg))  then return 2; end; "
        "  if not (int(E.Neg) <= int(E.Pos)) then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §10.5 (v0.314/#5398): same-enum ordering with no declared operator group is
// a compile error, with a diagnostic naming the three remedies.
BEZEL_TEST(enum_underlying_1189, same_enum_ordering_rejected_no_group) {
    auto r = runAot(
        "module M; "
        "enum Code: byte Lo := 10, Hi := 200; end Code; "
        "class C public static method Main(): int "
        "  if Code.Lo < Code.Hi then return 1; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is not defined for enum `Code`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "intrinsic ordering"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "int(a) < int(b)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "four-operator group"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "IComparable<Code>"));
}

// ── §10.3 (#4565): the colon-list HEAD gate ──────────────────────────────
//
// §10.3: "The colon list is one list: if its first item resolves to one of
// the eight underlying-type keywords, that item is the underlying type;
// otherwise the underlying type is `int` and every item names an interface.
// Where the otherwise branch then fails because the first item names a type
// that is not an interface — `enum Ratio: double` — the diagnostic should
// name the underlying-type fact."
//
// Pre-fix, TypeResolution_Names.cpp's enum-underlying-type arm fired on
// `ref->kind == TypeRef::Kind::Primitive` alone — ANY primitive (not just the
// eight integer kinds), at ANY colon-list position. That produced two
// distinct pre-fix failure modes, both closed by the same gate:
//   * `double`/`float`/`decimal` reached codegen carrying that "underlying
//     type", and enum equality's unconditional `CreateICmpEQ`
//     (LlvmBackend_Setup.cpp, emitEnumEquals) emitted an invalid `icmp` on a
//     float/struct operand — LLVM IR verification failure (measured: "Invalid
//     operand types for ICmp instruction" / "codegen error: IR verification
//     failed", a bare LLVM-verifier message with no `file:line:col:` prefix).
//   * `bool`/`char`/`string`/a class/a record were SILENTLY ACCEPTED (no
//     diagnostic at all, `ran == true`): `bool` (i1), `char` (an integer
//     width) and `string` (a pointer) are each a VALID icmp operand shape, so
//     they never tripped the verifier; a class or record is Kind::Named and
//     was pushed into the enum's `baseTypes` completely unexamined.
//
// FAIL-WITHOUT is asserted directly against these two distinct shapes rather
// than a bare `!ran` (issue #3892: vacuous for the silently-accepted shapes,
// which have `ran == true` pre-fix, not a different diagnostic) or
// `failedAtCompileTimeWithRealLocation` (CLAUDE.md's own caution: that helper
// can return true on both sides whenever a crash's backstop diagnostic
// happens to carry a location — here the pre-fix LLVM-verifier text has none,
// but the project's discipline is to assert the specific diagnostic anyway
// rather than lean on that).

BEZEL_TEST(enum_colon_list_underlying_type_4565, double_head_rejected_not_crashed) {
    // per §10.3 — pre-fix this reached codegen and crashed IR verification
    // ("icmp eq double"); post-fix it is a clean compile-time rejection
    // naming the underlying-type fact, not "IR verification failed".
    auto r = runAot(
        "module M; "
        "enum Ratio: double Half, Whole; end Ratio; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "admissible underlying types"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "malformed IR"));
}

BEZEL_TEST(enum_colon_list_underlying_type_4565, float_head_rejected_not_crashed) {
    // per §10.3 — `float` is Kind::Primitive but not one of the eight; pre-fix
    // it reached codegen and emitted `icmp eq float`.
    auto r = runAot(
        "module M; "
        "enum Ratio: float Half, Whole; end Ratio; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "admissible underlying types"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

BEZEL_TEST(enum_colon_list_underlying_type_4565, decimal_head_rejected_not_crashed) {
    // per §10.3 — `decimal` lowers to a struct (`%bz.decimal`); pre-fix
    // `icmp eq %bz.decimal` was rejected by the verifier as an
    // aggregate-typed ICmp operand.
    auto r = runAot(
        "module M; "
        "enum Ratio: decimal Half, Whole; end Ratio; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "admissible underlying types"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

BEZEL_TEST(enum_colon_list_underlying_type_4565, bool_head_now_rejected) {
    // per §10.3 — pre-fix `bool` (PrimitiveType::Bool, i1) was a VALID icmp
    // operand shape, so it was silently ACCEPTED as an underlying type with
    // NO diagnostic at all (`ran == true`, not a crash). The fix rejects it
    // outright: `bool` is not one of the eight, and is not an interface.
    auto r = runAot(
        "module M; "
        "enum Ratio: bool Half, Whole; end Ratio; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "admissible underlying types"));
}

BEZEL_TEST(enum_colon_list_underlying_type_4565, string_head_now_rejected) {
    // per §10.3 — pre-fix `string` (a pointer at codegen) was ALSO a valid
    // icmp operand shape, so a string-"backed" enum was silently accepted.
    auto r = runAot(
        "module M; "
        "enum Ratio: string Half, Whole; end Ratio; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "admissible underlying types"));
}

BEZEL_TEST(enum_colon_list_underlying_type_4565, class_head_now_rejected) {
    // per §10.3 — a class is Kind::Named with a declaration; nothing checked
    // it against the eight primitives OR against "is this an interface", so
    // it was pushed into the enum's `baseTypes` unexamined and accepted.
    auto r = runAot(
        "module M; "
        "class Box end Box; "
        "enum Ratio: Box Half, Whole; end Ratio; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "admissible underlying types"));
}

BEZEL_TEST(enum_colon_list_underlying_type_4565, record_head_now_rejected) {
    // per §10.3 — the same gap as the class case above, for a record head.
    auto r = runAot(
        "module M; "
        "record Point(x: int, y: int); "
        "enum Ratio: Point Half, Whole; end Ratio; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "admissible underlying types"));
}

// Regression guards: a real interface head, and the eight admissible
// primitives (by keyword and by reserved PascalCase alias), are unaffected.
BEZEL_TEST(enum_colon_list_underlying_type_4565, real_interface_head_still_accepted) {
    // per §10.3 — a genuine interface at colon-list position 0 is admitted;
    // the underlying type defaults to `int` (§10.3's "otherwise" branch).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface ITagged method Tag(): int; end ITagged; "
        "enum Ratio: ITagged "
        "  Half, Whole; "
        "  public method Tag(): int => 0; "
        "end Ratio; "
        "class C public static method Main(): int "
        "  return (if int(Ratio.Half) = 0 then 0 else 1); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(enum_colon_list_underlying_type_4565, pascalcase_alias_head_still_accepted) {
    // per §10.3 / §3.4 — `Int16` (the reserved PascalCase alias for `short`)
    // is one of the eight, spelled differently: `enum E: Int16` and
    // `enum E: short` declare the same type.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Ratio: Int16 Half, Whole := 30000; end Ratio; "
        "class C public static method Main(): int "
        "  return (if int(Ratio.Whole) = 30000 then 0 else 1); "
        "end Main; end C;"), 0);
}
