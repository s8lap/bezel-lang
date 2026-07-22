// fix #849 / §18.5 / §7.1: string interpolation of nullable value types was
// broken in three ways. (1) A format spec on a nullable value (`$"{v:X}"`,
// `$"{v:F2}"` with `v: int?`) fed the `{i8, T}` tagged struct straight to
// emitWithFormatSpec, which sign-extended a struct (`{i8,i32}` → i64, an IR
// verification failure) or dropped the spec. The interpolation now branches on
// hasValue and applies the spec to the UNWRAPPED payload (absent → "null").
// (2) `char?` was excluded from isNullablePrimTy (lumped with the reference
// primitives), so it was classified as neither a nullable value nor a nullable
// reference — a null `char?` interpolated as a NUL byte instead of "null", and
// `c ?? x` crashed with a PHI type mismatch. `char` is a value type (i32), so
// `char?` is now a proper `{i8 hasValue; i32}` nullable value type.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// bug14: `$"{v:X}"` with `v: int? = 255` formats as "FF" (was a sext-on-struct
// IR verification failure).
BEZEL_TEST(nullable_value_format_interp_849, int_nullable_hex_format) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var v: int? := 255; var s := $\"{v:X}\"; "
        "  return (if s = \"FF\" then 1 else 2); "
        "end Main; end C;"), 1);
}

// bug15: `$"{v:F2}"` with `v: int? = 42` formats as "42.00" (was silently
// dropped → "42").
BEZEL_TEST(nullable_value_format_interp_849, int_nullable_fixed_format) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var v: int? := 42; var s := $\"{v:F2}\"; "
        "  return (if s = \"42.00\" then 1 else 2); "
        "end Main; end C;"), 1);
}

// A null nullable value WITH a format spec renders the literal "null".
BEZEL_TEST(nullable_value_format_interp_849, null_with_format_renders_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var v: int? := null; var s := $\"{v:X}\"; "
        "  return (if s = \"null\" then 1 else 2); "
        "end Main; end C;"), 1);
}

// double? with a fixed-point spec.
BEZEL_TEST(nullable_value_format_interp_849, double_nullable_fixed_format) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var v: double? := 3.14159; var s := $\"{v:F2}\"; "
        "  return (if s = \"3.14\" then 1 else 2); "
        "end Main; end C;"), 1);
}

// bug16: a null `char?` interpolates as the literal "null", not a NUL byte.
BEZEL_TEST(nullable_value_format_interp_849, null_char_renders_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var c: char? := null; var s := $\"{c}\"; "
        "  return (if s = \"null\" then 1 else 2); "
        "end Main; end C;"), 1);
}

// `char?` is now a proper nullable value type: `c ?? x` no longer crashes (was a
// PHI type mismatch from the inconsistent representation).
BEZEL_TEST(nullable_value_format_interp_849, char_nullable_coalesce) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var c: char? := null; var d: char := c ?? 'Z'; "
        "  return int(d); "                 // 90
        "end Main; end C;"), 90);
}

// Regression: `char?` null-check and force-unwrap still work.
BEZEL_TEST(nullable_value_format_interp_849, char_nullable_isnull_and_unwrap) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: char? := null; "
        "  var b: char? := 'A'; "
        "  if a is not null then return 99; end; "
        "  if b is null then return 98; end; "
        "  return int(b!); "                // 65
        "end Main; end C;"), 65);
}
