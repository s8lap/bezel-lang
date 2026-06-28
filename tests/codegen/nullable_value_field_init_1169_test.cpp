// fix #1169 / §23.1 / §7: a FIELD or auto-property of nullable VALUE type with an
// inline initializer (`int? := 42`, `bool? := true`, a nullable record
// `Pt? := new Pt(3,4)`) lowers to a tagged {i8 hasValue; T value} slot, but
// emitOwnFieldInit stored the bare inner value UNWRAPPED into that slot — the
// scalar's low byte clobbered the hasValue tag (read back as present-but-zero;
// the record case corrupted the payload). The fix wraps/widens the initializer
// to the tagged struct, exactly as a constructor-body assignment and the
// method-return path (coerceReturnToNullableValue) already do. A reference-typed
// nullable (`string? := "hi"`) is NOT a value type and is left untouched.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); test values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1169: a nullable VALUE-type FIELD initializer (`int? := 42`) round-trips —
// the field is present (not null) and reads back 42 (was 0).
BEZEL_TEST(nullable_value_field_initializer_roundtrips_1169, int_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class H "
        "  private _x: int? := 42; "
        "  public constructor() end constructor; "
        "  public method Val(): int => _x!; "
        "  public method IsNull(): bool => _x = null; "
        "end H; "
        "class P public static method Main(): int "
        "  using var h := new H(); "
        "  if h.IsNull() then return 200; end; "          // present, not null
        "  return h.Val(); "                              // 42, was 0
        "end Main; end P;"), 42);
}

// The same through an auto-property (`property M: int? := 42`).
BEZEL_TEST(nullable_value_field_initializer_roundtrips_1169, int_auto_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; class H "
        "  public property Maybe: int? := 42; "
        "  public constructor() end constructor; "
        "  public method Val(): int => Maybe!; "
        "  public method IsNull(): bool => Maybe = null; "
        "end H; "
        "class P public static method Main(): int "
        "  using var h := new H(); "
        "  if h.IsNull() then return 200; end; "
        "  return h.Val(); "
        "end Main; end P;"), 42);
}

// A `bool? := true` field: the i1 value is wrapped, so the read sees `true`
// (was stored unwrapped → read back false).
BEZEL_TEST(nullable_value_field_initializer_roundtrips_1169, bool_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class H "
        "  private _b: bool? := true; "
        "  public constructor() end constructor; "
        "  public method Val(): int "
        "    if _b! then return 7; end; "
        "    return 9; "
        "  end Val; "
        "  public method IsNull(): bool => _b = null; "
        "end H; "
        "class P public static method Main(): int "
        "  using var h := new H(); "
        "  if h.IsNull() then return 200; end; "
        "  return h.Val(); "                              // 7 (true), was 9
        "end Main; end P;"), 7);
}

// A nullable RECORD field (`Pt? := new Pt(3, 4)`): the bare record value is
// wrapped into the {i8, %record.Pt} slot, so both fields decode (was corrupt).
BEZEL_TEST(nullable_value_field_initializer_roundtrips_1169, record_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class H "
        "  private _p: Pt? := new Pt(3, 4); "
        "  public constructor() end constructor; "
        "  public method Sum(): int => _p!.X + _p!.Y; "
        "  public method IsNull(): bool => _p = null; "
        "end H; "
        "class P public static method Main(): int "
        "  using var h := new H(); "
        "  if h.IsNull() then return 200; end; "
        "  return h.Sum(); "                              // 7, was corrupt
        "end Main; end P;"), 7);
}

// A class with NO explicit constructor still wraps the initializer (the
// no-ctor path runs emitOwnFieldInit via emitInstanceInit).
BEZEL_TEST(nullable_value_field_initializer_roundtrips_1169, no_explicit_ctor) {
    BEZEL_EXPECT_EQ(run(
        "module M; class H "
        "  private _x: int? := 99; "
        "  public method Val(): int => _x!; "
        "end H; "
        "class P public static method Main(): int "
        "  using var h := new H(); "
        "  return h.Val(); "                              // 99
        "end Main; end P;"), 99);
}

// CONTROL: a REFERENCE-typed nullable field (`string? := "hi"`) is not a value
// type, so the wrap is skipped and it keeps working (reads its length).
BEZEL_TEST(nullable_value_field_initializer_roundtrips_1169, string_field_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class H "
        "  private _s: string? := \"hi\"; "
        "  public constructor() end constructor; "
        "  public method Len(): int => _s!.Length; "
        "  public method IsNull(): bool => _s = null; "
        "end H; "
        "class P public static method Main(): int "
        "  using var h := new H(); "
        "  if h.IsNull() then return 200; end; "
        "  return h.Len(); "                              // 2
        "end Main; end P;"), 2);
}

// CONTROL: a NON-nullable value-type field initializer is unchanged.
BEZEL_TEST(nullable_value_field_initializer_roundtrips_1169, non_nullable_field_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class H "
        "  private _x: int := 42; "
        "  public constructor() end constructor; "
        "  public method Val(): int => _x; "
        "end H; "
        "class P public static method Main(): int "
        "  using var h := new H(); "
        "  return h.Val(); "                              // 42
        "end Main; end P;"), 42);
}

// CONTROL: an UNINITIALIZED nullable value field still reads as null (the wrap
// must not introduce a spurious value — calloc-zero stays {0, 0}).
BEZEL_TEST(nullable_value_field_initializer_roundtrips_1169, uninitialized_stays_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class H "
        "  private _x: int?; "
        "  public constructor() end constructor; "
        "  public method IsNull(): bool => _x = null; "
        "end H; "
        "class P public static method Main(): int "
        "  using var h := new H(); "
        "  if h.IsNull() then return 99; end; "           // null → 99
        "  return 1; "
        "end Main; end P;"), 99);
}
