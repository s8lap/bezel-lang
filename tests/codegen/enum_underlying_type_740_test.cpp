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

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
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
        "  return (0 if b = 9000000000L else 1); "
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
        "  return case x Big.A => 1, Big.B => 2, Big.C => 3 end; "
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
        "  return (0 if b = Big.B else 1); "
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
        "  return (0 if int(E.Neg) = -100 else 1); "
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
        "  return (0 if d[Big.B] = 42 else 1); "
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
        "  return (0 if Big.B.Doubled() = 18000000000L else 1); "
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
        "  return (0 if x.ToString() = \"Big\" else 1); "
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
        "  return case d Dir.North => 10, Dir.East => 0, _ => 9 end; "
        "end Main; end C;"), 0);
}
