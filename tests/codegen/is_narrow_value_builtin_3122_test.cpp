// Codegen + sema — §12.7, #3122: a read through a §12.7 flow-narrowing to a
// VALUE-BUILTIN (DateTime / TimeSpan / DateOnly / TimeOnly / Guid) must produce
// the boxed VALUE, not the box POINTER.
//
// spec §12.7 (v0.252), *Type tests: `is`, `is not`, `as`*:
//   "`is T` tests whether a value is an instance of `T` and produces `bool`.
//    When the test succeeds and the tested expression is a simple variable, the
//    variable is **flow-narrowed** to `T` within the controlled block:
//      if animal is Dog then
//        animal.Bark();        // animal narrowed to Dog
//      end;"
//
//   "Both `is` and `as` are **non-destructive on their operand**: the operation
//    inspects the runtime type tag without moving from the operand."
//
// spec §8.6 (v0.252), *Boxing*:
//   "When a value type is used where `Object` (or an interface implemented by
//    the value type) is expected, the value is **boxed**: a heap-allocated
//    wrapper holds the value."
//
// THE DEFECT — two halves, both required (the value-builtin half #3090
// explicitly deferred because it lives upstream of the codegen gate).
//
//   (1) SEMA's narrowable test-type set is a POSITIVE list — Named (with a
//       declaration), Primitive, InlineArray. A value-builtin written as an
//       ANNOTATION resolves to the declaration-less Named shape — the SAME
//       shape a bare type PARAMETER (`T` in `Box<T>`) resolves to. The two
//       cases are distinguished by NAME, not by shape: valueBuiltinBoxNameStr
//       in codegen already recognises the five boxable value-builtin names
//       across all three TypeRef spellings (#1945/#2809). Without that name
//       test in sema, `o is DateTime` narrowed NOTHING — the copy-out
//       `var d: DateTime := o;` then fell through to the declared `Object`'s
//       permissive acceptance (#605/#3115) and stored the BOX POINTER into a
//       slot the DateTime intrinsics read as an `i64` tick value. MEASURED on
//       the pre-fix compiler (this commit):
//
//         internal: codegen cannot read member 'Year' on receiver of type
//         'Object': permissive acceptance (spec §3 / issue #605) ...
//         Call parameter type does not match function signature!
//           %Year = call i32 @bz_datetime_year(ptr %5)
//         bezelc: codegen error: IR verification failed (malformed IR emitted)
//
//       Same shape, same pre-fix error: no `is` guard at all. Both pin the
//       cause on the missing narrowing, not on anything #3090 touched.
//
//   (2) CODEGEN already had the unbox path — `isBoxableValueType` returns true
//       for a value-builtin via `valueBuiltinBoxName`, `boxValue` stores the
//       payload at the standard two-word offset, and `narrowedBoxPayloadAddr`
//       reads from it. So the only change needed is widening sema's gate to
//       admit a declaration-less Named TypeRef when its head names a boxable
//       value-builtin (the issue's suggested fix, verbatim); codegen's #3090
//       unbox then applies unchanged.
//
// WHY THE TWO SHAPES (value-builtin annotation / bare type parameter) NEED A
// NAME TEST, NOT A KIND TEST: a declaration-less Named TypeRef is the storage
// shape both spells produce after resolution. `Box<T>`'s `T` and a value-
// builtin annotation `DateTime` are structurally the same TypeRef — they
// differ only in the name the use site spells. Admitting one by shape would
// admit both and #3092 (the bare-type-parameter case, which genuinely MUST
// NOT narrow because the body is unspecialized and `emitIsA` has no concrete
// answer for an unbounded `T`) would regress. The five-name set
// (DateTime/TimeSpan/DateOnly/TimeOnly/Guid) is the only distinction.
//
// AXIS GRID, walked below:
//   * DateTime     — the canonical value-builtin (§2.5, i64 tick payload).
//   * Guid         — multi-field payload ({i64, i64}), the only value-builtin
//                    whose bare lowering has no null representation (the
//                    nullable-wrapper GEP crash #1954 fixed).
//   * TimeSpan     — the second i64-payload value-builtin; rounds out the
//                    set #3090 deferred.
//   * user record  — user-defined value-type struct (Pt). Already narrowed
//                    before this fix because its declaration-bearing Named
//                    shape was already on the positive list; included here
//                    so the regression pins the SURFACE, not just the fix's
//                    own kind — `record` must keep working.
//   * user class   — narrowing a CLASS type narrows to a reference (the
//                    existing code path); the read must be the bare pointer,
//                    not the boxed payload. Verifies the new gate does not
//                    accidentally start reading payload for class targets.
//   * interface    — narrowing from an interface location to a value-type
//                    record; the value-side half of the #3090 interface
//                    location test, kept here for completeness.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const char* src) {
    JitResult r = runAot(src);
    if (!r.ran) return -1;
    return r.exitCode;
}

// =====================================================================
// (1) DateTime narrowing — the canonical value-builtin.
// =====================================================================

// §12.7 + §8.6: the spec-stated spelling for reading a value-builtin back out
// of an Object — `var d: T := o` after `if o is T`. Pre-fix: "IR verification
// failed" (the box pointer stored as a DateTime tick). Want 20240615.
BEZEL_TEST(codegen_is_narrow_value_builtin_3122,
           datetime_narrowing_copy_out_yields_the_value_spec_12_7_8_6) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := new DateTime(2024, 6, 15); "
        "    if o is DateTime then "
        "      var d: DateTime := o; "
        "      Console.WriteLine(d.Year * 10000 + d.Month * 100 + d.Day); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("20240615"));
}

// §12.7: the DIRECT member read, no copy-out local — exercises the same
// payload-address path through a different lowering (`fieldAddr`'s record arm
// on the value-typed narrowed binding). Want 20240615.
BEZEL_TEST(codegen_is_narrow_value_builtin_3122,
           datetime_narrowing_direct_member_read_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := new DateTime(2024, 6, 15); "
        "    if o is DateTime then "
        "      Console.WriteLine(o.Year * 10000 + o.Month * 100 + o.Day); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("20240615"));
}

// =====================================================================
// (2) Guid narrowing — multi-field {i64, i64} payload, the hardest of the
// five (#1954 / #1945 territory).
// =====================================================================

// §12.7 + §8.6: a Guid boxes into Object like a record, but its payload is
// {i64, i64} — the same two-word offset the read uses. Pre-fix: same
// malformed-IR shape as DateTime. Want 36 (the canonical D-form length).
BEZEL_TEST(codegen_is_narrow_value_builtin_3122,
           guid_narrowing_copy_out_yields_the_value_spec_12_7_8_6) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := Guid.NewGuid(); "
        "    if o is Guid then "
        "      var g: Guid := o; "
        "      var s: string := g.ToString(); "
        "      Console.WriteLine(s.Length); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("36"));
}

// §12.7: Guid.EqualS (the IEquatable<Guid> override). Two Guids boxed into
// Object, narrowed back, compared — pre-fix the comparison read the box
// pointer instead of the {i64, i64} payload, so even two Guids of the same
// value would fail equality on the narrowed read. Want 1.
BEZEL_TEST(codegen_is_narrow_value_builtin_3122,
           guid_narrowing_equality_uses_payload_not_pointer_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: Object := Guid.Empty; "
        "    var b: Object := Guid.Empty; "
        "    if a is Guid and b is Guid then "
        "      var ga: Guid := a; "
        "      var gb: Guid := b; "
        "      if ga.Equals(gb) then return 1; end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// =====================================================================
// (3) TimeSpan — the second i64-payload value-builtin (round-trip).
// =====================================================================

// §12.7 + §8.6: a TimeSpan's payload is also an i64 (#1945), so the same
// unbox path applies. Returns 1 — the narrowing fires and the value-builtin
// is read out (any other path would have produced malformed IR or a wrong
// value; the pre-fix shape is the DateTime IR-verification failure).
// NOTE: A `Ticks`/`TotalSeconds`-as-string assertion is NOT made here
// because the Console.WriteLine(long) surface has its own pre-existing
// quirk independent of this fix (verified on baseline: `var t := new
// TimeSpan(1, 1, 1); Console.WriteLine(t.Ticks);` does not print the
// expected `36610000000`). The narrowing + read here exercises the same
// unbox path as DateTime/Guid and pins the §12.7 flow-narrowing surface.
BEZEL_TEST(codegen_is_narrow_value_builtin_3122,
           timespan_narrowing_copy_out_compiles_and_executes_spec_12_7_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := new TimeSpan(1, 1, 1); "
        "    if o is TimeSpan then "
        "      var t: TimeSpan := o; "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// =====================================================================
// Axis-grid coverage — keeps the new gate from regressing adjacent shapes.
// =====================================================================

// §12.7: a user-defined VALUE-TYPE RECORD. Already worked before this fix
// (declaration-ful Named TypeRef), pinned here so the regression covers the
// SURFACE, not just the fix's kind. Pre-fix: 86 = 7*10 + 9 + 7 (X*10 + Y +
// direct o.X).
BEZEL_TEST(codegen_is_narrow_value_builtin_3122,
           user_record_narrowing_unchanged_spec_12_7_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(7, 9); "
        "    var o: Object := p; "
        "    if o is Pt then "
        "      var q: Pt := o; "
        "      return q.X * 10 + q.Y + o.X; "
        "    end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 86);
}

// §12.7: a user-defined CLASS — narrowing a CLASS target narrows to a
// reference, so the read must be the bare pointer (the existing path), NOT
// the boxed payload. This is the guard that the value-builtin gate does not
// accidentally start reading payload for class targets. Want 1 — proves the
// narrowing happened AND the variable can be used as the narrowed class type
// (the `new Bar()` allocation + narrowing succeeds, the if-branch is taken).
BEZEL_TEST(codegen_is_narrow_value_builtin_3122,
           user_class_narrowing_stays_a_pointer_read_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFoo "
        "  method X(): int; "
        "end IFoo; "
        "class Foo: IFoo "
        "  public method X(): int => 7; "
        "end Foo; "
        "class Bar: IFoo "
        "  public method X(): int => 99; "
        "end Bar; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := new Bar(); "
        "    if o is IFoo then "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §12.7: `is not T`'s else branch narrows too — the spec wording is "the
// variable is narrowed to `T` in the else branch". For a value-builtin, the
// same unbox path applies through the else branch. The test deliberately
// puts the value-builtin on the FALSE side of `is not`, so the narrowing
// fires in the else branch and the read there must produce the year.
// Want 20240615.
BEZEL_TEST(codegen_is_narrow_value_builtin_3122,
           datetime_narrowing_in_is_not_else_branch_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := new DateTime(2024, 6, 15); "
        "    if o is not DateTime then "
        "      Console.WriteLine(\"no\"); "
        "    else "
        "      var d: DateTime := o; "
        "      Console.WriteLine(d.Year * 10000 + d.Month * 100 + d.Day); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("20240615"));
}

// §12.7: an `Object` PARAMETER location — sema keys narrowing on
// LocalSymbol OR ParameterSymbol, and codegen routes a parameter's storage
// through `paramSlots_` / `refParamAddr`, a different path from a local's
// `locals_`. Combined: narrowing on an Object parameter to DateTime. Want
// 20240615.
BEZEL_TEST(codegen_is_narrow_value_builtin_3122,
           datetime_narrowing_on_an_object_parameter_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Day(o: Object): int "
        "    if o is DateTime then "
        "      var d: DateTime := o; "
        "      return d.Day; "
        "    end; "
        "    return 0; "
        "  end Day; "
        "  public static method Main(): int "
        "    Console.WriteLine(Day(new DateTime(2024, 6, 15))); "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("15"));
}

}  // namespace
