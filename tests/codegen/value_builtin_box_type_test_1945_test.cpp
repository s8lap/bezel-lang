// fix #1945 / §8.6 + §19.3 (v0.182): a boxed VALUE-BUILTIN (DateTime / TimeSpan /
// DateOnly / TimeOnly / Guid) had no reference/box descriptor, so:
//   (a) `case o of DateTime` / `o is DateTime` NEVER matched even when the Object
//       genuinely held a boxed DateTime (a silent wrong-branch miscompile), and
//   (b) boxing a value-builtin into Object emitted MALFORMED IR in some call
//       shapes (e.g. `Kind(od)` where Kind takes an Object) — the raw scalar
//       (i64) was passed where a box pointer was expected → IR verification
//       failure, a hard codegen abort.
//
// Root cause (src/codegen): value-builtins are scalar/struct value types
// (DateTime/TimeSpan → i64, DateOnly/TimeOnly → i32, Guid → {i64,i64}) that
// isBoxableValueType did NOT recognise, so boxValue was a no-op (leaving the raw
// scalar in an Object slot → (b)), and neither boxDescriptorFor nor
// descriptorForPatternName resolved a box descriptor for them (→ (a)).
//
// Fix: value-builtins now box like a record — isBoxableValueType/boxValue wrap
// them into `{ vtable*, rc, payload }`, and their box vtable's type tag reuses the
// value-builtin's EXISTING class descriptor (td$<name>, already in descArray_) so
// it compares equal in emitIsA to the descriptor descriptorForPatternName /
// boxDescriptorFor resolve for the type-test target. §8.6 boxing + §19.3 type
// tests then work uniformly with primitives and records.
//
// This also lets the §19.3 v0.182 (#1852) TYPE-PARAMETER form `case o of T`
// (T=DateTime) match a boxed value-builtin — previously a documented well-formed
// never-match precisely because this descriptor was missing.
//
// Each test pins the defect: WITHOUT the fix the (a)/type-param cases return the
// non-match arm (wrong value) and the (b) case aborts codegen on malformed IR
// (runAot → ran==false → -999); WITH the fix each returns the spec-stated value.
// Observable is the process exit code (Main's returned int).

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

// -- Defect (a): concrete `case o of DateTime` over a boxed DateTime -----------

// §8.6 / §19.3 (#1945): a boxed DateTime matches its own type-test → 1. Without
// the fix the boxed value carried no descriptor and the arm never fired → 0.
BEZEL_TEST(value_builtin_box_type_test_1945, case_boxed_datetime_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var dt: DateTime := DateTime.UtcNow; "
        "    var o: Object := dt; "
        "    return (case o of DateTime => 1, _ => 0); "
        "  end Main; end Program;"), 1);
}

// §8.6 / §19.3 (#1945): a boxed DateTime is NOT a Guid — a different value-builtin
// type-test must never-match → 0 (correct discrimination, not a blanket match).
BEZEL_TEST(value_builtin_box_type_test_1945, case_boxed_datetime_not_guid) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var dt: DateTime := DateTime.UtcNow; "
        "    var o: Object := dt; "
        "    return (case o of Guid => 1, _ => 0); "
        "  end Main; end Program;"), 0);
}

// -- Defect (b): DateTime -> Object boxing across a call boundary --------------

// §8.6 (#1945): passing a boxed value-builtin to a static method taking `Object`
// is the exact call shape that emitted malformed IR (i64 passed where a box ptr
// was expected). It now compiles AND the boxed DateTime matches inside the callee
// → 2. Without the fix codegen aborts (IR verification failed) → -999.
BEZEL_TEST(value_builtin_box_type_test_1945, boxed_datetime_object_argument_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Kind(o: Object): int "
        "    return (case o of DateTime => 2, _ => 9); "
        "  end Kind; "
        "  public static method Main(): int "
        "    var dt: DateTime := DateTime.UtcNow; "
        "    var od: Object := dt; "
        "    return Kind(od); "
        "  end Main; end Program;"), 2);
}

// -- The `is` / `is not` operator (§12.7) over a boxed value-builtin -----------

// §12.7 (#1945): `o is DateTime` is true and `o is Guid` is false for a boxed
// DateTime. Encodes both as 1 (true) + 100 (would-be-wrong Guid match) → 1.
BEZEL_TEST(value_builtin_box_type_test_1945, is_operator_boxed_datetime) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var dt: DateTime := DateTime.UtcNow; "
        "    var o: Object := dt; "
        "    var r: int := 0; "
        "    if o is DateTime then r := r + 1; end; "
        "    if o is Guid then r := r + 100; end; "
        "    return r; "
        "  end Main; end Program;"), 1);
}

// -- Unbox round-trip: `o as DateTime` (§17.4) --------------------------------

// §8.6 / §17.4 (#1945): a boxed DateTime unboxes back to the SAME value — the box
// payload is the DateTime ticks, so `(o as DateTime) = dt`. Returns 5 on a
// value-preserving round-trip, 100 if the unbox produced null.
BEZEL_TEST(value_builtin_box_type_test_1945, as_unbox_datetime_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var dt: DateTime := DateTime.UtcNow; "
        "    var o: Object := dt; "
        "    var back: DateTime? := o as DateTime; "
        "    if back = null then return 100; end; "
        "    if back = dt then return 5; end; "
        "    return 6; "
        "  end Main; end Program;"), 5);
}

// -- Other value-builtins: Guid, TimeSpan -------------------------------------

// §8.6 / §19.3 (#1945): a boxed Guid matches `case o of Guid` (10) and does NOT
// match `case o of DateTime` (0) → 10.
BEZEL_TEST(value_builtin_box_type_test_1945, case_boxed_guid_matches_not_datetime) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var g: Guid := Guid.NewGuid(); "
        "    var o: Object := g; "
        "    var a: int := (case o of Guid => 10, _ => 0); "
        "    var b: int := (case o of DateTime => 1, _ => 0); "
        "    return a + b; "
        "  end Main; end Program;"), 10);
}

// §8.6 / §19.3 (#1945): a boxed TimeSpan matches `case o of TimeSpan` → 7.
BEZEL_TEST(value_builtin_box_type_test_1945, case_boxed_timespan_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Program "
        "  public static method Main(): int "
        "    var t: TimeSpan := TimeSpan.FromSeconds(5.0); "
        "    var o: Object := t; "
        "    return (case o of TimeSpan => 7, _ => 0); "
        "  end Main; end Program;"), 7);
}

// -- #1852 dependency: the §19.3 TYPE-PARAMETER form now matches ----------------

// §19.3 (v0.182) / #1852 + #1945: `case o of T` with T=DateTime over a boxed
// DateTime scrutinee now runs a REAL is-a and matches → 2. This was a documented
// well-formed never-match (9) in #1852 precisely because the box descriptor was
// missing; #1945 supplies it, so the type-parameter form matches like the
// concrete `case o of DateTime`.
BEZEL_TEST(value_builtin_box_type_test_1945, type_param_datetime_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  public method Kind(o: Object): int "
        "    return (case o of T => 2, _ => 9); "
        "  end Kind; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box<DateTime>(); "
        "  var dt: DateTime := DateTime.UtcNow; "
        "  var o: Object := dt; "
        "  return b.Kind(o); "
        "end Main; end Program;"), 2);
}

// §19.3 (v0.182) / #1852 + #1945: the type-parameter form still DISCRIMINATES — a
// boxed DateTime against T=Guid must never-match → 9 (no blanket value-builtin
// match).
BEZEL_TEST(value_builtin_box_type_test_1945, type_param_guid_rejects_datetime) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  public method Kind(o: Object): int "
        "    return (case o of T => 2, _ => 9); "
        "  end Kind; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box<Guid>(); "
        "  var dt: DateTime := DateTime.UtcNow; "
        "  var o: Object := dt; "
        "  return b.Kind(o); "
        "end Main; end Program;"), 9);
}
