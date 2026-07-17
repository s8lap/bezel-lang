// fix #1954 / stdlib §2.5–§2.6 + spec §7.2 / §17.4 (v0.197): a NULLABLE
// value-builtin (`Guid?` / `DateTime?` / `TimeSpan?` / `DateOnly?` / `TimeOnly?`)
// had no tagged-nullable lowering. Value-builtins are scalar/struct value types
// (DateTime/TimeSpan → i64, DateOnly/TimeOnly → i32, Guid → {i64,i64}) whose decl
// kind is Class (or declaration-less), so isNullableValueTy — the union of
// isNullablePrimTy / isNullableEnumTy / isNullableRecordTy — did NOT recognise them.
//
// A `Guid?` therefore fell through to the BARE {i64,i64} lowering with NO null
// representation, and the nullable-wrapper GEP built over it was malformed:
//   Instructions.h:942 checkGEPType: Assertion 'Ty && "Invalid GetElementPtrInst
//   indices for type!"' failed  — a HARD compiler abort (core dump) on VALID source
// (no Object/boxing/`as` needed — the underlying gap). The single-word builtins
// (DateTime/TimeSpan → i64, DateOnly/TimeOnly → i32) did not crash but silently
// dropped nullability (a null `Guid?`/`DateTime?` was indistinguishable from a
// value).
//
// Root cause (src/codegen): isNullableValueTy did not cover value-builtins, so
// neither nullableValueStructTy nor any of the null/wrap/compare sites keyed off it
// treated a `Guid?` as the tagged {i8 hasValue; T value} optional.
//
// Fix: a new isNullableValueBuiltinTy predicate (delegating to valueBuiltinName)
// joins isNullableValueTy, and nullableValueStructTy lowers a nullable value-builtin
// to {i8 hasValue; <non-nullable builtin lowering>} — exactly as records/enums do,
// so `null` is representable and distinct from every value (critically the
// multi-field Guid {i64,i64}).
//
// This fix also resolves #1953: the §17.4 `as` unbox of a value-builtin now routes
// through the nullable-tagged branch, so a FAILED value-builtin `as` yields `null`
// (not the zero value / MinValue), and a matching `as` round-trips through the
// nullable.
//
// Each test pins the defect: WITHOUT the fix the Guid? cases ABORT codegen (runAot
// → ran==false → -999) and the #1953 as-null case returns the wrong value; WITH the
// fix each returns the spec-stated value. Observable is Main's returned int.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// -- The exact crash repro: a `Guid?` null-check (NO Object/boxing/`as`) ---------

// stdlib §2.6 / spec §7.2 (#1954): `var back: Guid? := g; if back = null …` — the
// multi-field Guid {i64,i64} nullable. Without the fix the malformed nullable-
// wrapper GEP aborts the compiler (checkGEPType assertion → -999); with it the
// has-value case is distinguished from null → 7.
BEZEL_TEST(nullable_value_builtin_1954, guid_nullable_null_check_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var g: Guid := Guid.NewGuid(); "
        "    var back: Guid? := g; "
        "    if back = null then return 1; end; "     // has value → must not fire
        "    return 7; "
        "  end Main; end Program;"), 7);
}

// stdlib §2.6 / spec §7.2 (#1954): a `Guid?` round-trips its value AND a real null
// `Guid?` compares equal to null. Encodes: has-value(+1) & value-preserved(+10) &
// real-null-is-null(+100) → 111. Without the fix codegen aborts → -999.
BEZEL_TEST(nullable_value_builtin_1954, guid_nullable_value_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var g: Guid := Guid.NewGuid(); "
        "    var back: Guid? := g; "
        "    var acc: int := 0; "
        "    if back <> null then acc := acc + 1; end; "     // present
        "    if back = g then acc := acc + 10; end; "        // value preserved
        "    var nn: Guid? := null; "
        "    if nn = null then acc := acc + 100; end; "      // real null
        "    if nn <> null then acc := acc + 1000; end; "    // must not fire
        "    return acc; "
        "  end Main; end Program;"), 111);
}

// -- Other value-builtins: DateTime? / TimeSpan? / DateOnly? nullable -----------

// stdlib §2.5 / spec §7.2 (#1954): a `DateTime?` (single-word i64) round-trips its
// value and a null `DateTime?` is distinct from any value → 3.
BEZEL_TEST(nullable_value_builtin_1954, datetime_nullable_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var dt: DateTime := DateTime.UtcNow; "
        "    var d: DateTime? := dt; "
        "    if d = null then return 11; end; "        // present
        "    if d <> dt then return 12; end; "         // value preserved
        "    var nn: DateTime? := null; "
        "    if nn <> null then return 13; end; "      // real null
        "    return 3; "
        "  end Main; end Program;"), 3);
}

// stdlib §2.5 / spec §7.2 (#1954): a `TimeSpan?` and a null `DateOnly?` — the other
// single-word (TimeSpan → i64) and i32 (DateOnly) value-builtins → 4.
BEZEL_TEST(nullable_value_builtin_1954, timespan_dateonly_nullable) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var ts: TimeSpan := TimeSpan.FromSeconds(5.0); "
        "    var tsn: TimeSpan? := ts; "
        "    if tsn = null then return 21; end; "      // present
        "    if tsn <> ts then return 22; end; "       // value preserved
        "    var don: DateOnly? := null; "
        "    if don <> null then return 23; end; "     // real null
        "    return 4; "
        "  end Main; end Program;"), 4);
}

// -- #1953: a FAILED value-builtin `as` yields null (§17.4), not MinValue ---------

// spec §17.4 (#1953): "the result of `as` is always nullable; a failed cast yields
// null." A Guid boxed into Object is NOT a DateTime, so `o as DateTime` must be
// null. Without the fix the value-builtin `as` returned the bare zero value
// (DateTime MinValue, not null) → 0; with it → 1.
BEZEL_TEST(nullable_value_builtin_1954, as_failed_value_builtin_cast_is_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var g: Guid := Guid.NewGuid(); "
        "    var o: Object := g; "
        "    var back: DateTime? := o as DateTime; "   // Guid is not DateTime → null
        "    if back = null then return 1; end; "
        "    return 0; "
        "  end Main; end Program;"), 1);
}

// spec §17.4 / stdlib §2.6 (#1954/#1953): a MATCHING value-builtin `as Guid`
// unboxes and round-trips through the resulting `Guid?` (the {i64,i64} payload).
// Without the nullable-value-builtin lowering the `Guid?` result crashed codegen
// (checkGEPType → -999); with it the round-trip value is preserved → 5.
BEZEL_TEST(nullable_value_builtin_1954, as_matching_guid_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var g: Guid := Guid.NewGuid(); "
        "    var o: Object := g; "
        "    var back: Guid? := o as Guid; "
        "    if back = null then return 100; end; "    // matching cast → non-null
        "    if back = g then return 5; end; "         // value preserved
        "    return 6; "
        "  end Main; end Program;"), 5);
}
