// Codegen — §12.7 "A nullable target tests as its underlying type" / §17.4
// "`as` on value types" / §8.6 "Boxing" / stdlib §2.5-§2.6 / #8593
// (spec v0.357): unboxing a BOXED built-in VALUE type with `as T?` (and
// testing it with `is T?`) must answer for the UNDERLYING type.
//
// THE DEFECT (MEASURED on the parent, r30-c1 @ 4d689965e):
//   `var g: IComparable<Guid> := Guid.Empty; var x: Guid? := g as Guid?;`
//   -> "internal: lowerLocalDecl's initializer expression failed to lower to
//      a value for this local's slot (degraded to a zero-initialized value) —
//      #2434", then `x <> null` printed `False` where §17.4 requires `True`.
//   The `DateTime?` twin failed identically. The `is` twin (`o is Guid?`)
//   dropped the test altogether ("internal: the argument of this
//   `Console.WriteLine` failed to lower to a value … — #3079") and printed
//   NOTHING.
//
// THE CAUSE. §12.7: "A nullable target tests as its underlying type: `x is
// T?` has the answer of `x is T` and narrows as it does … and `x as T?` is
// `x as T`." So the TARGET's nullability changes nothing about what the test
// looks for. But `valueBuiltinBoxName` deliberately EXCLUDES a nullable
// source (a `Guid?` → Object box is a separate shape, keyed by
// `isNullableValueBuiltinTy`), so the NULLABLE VALUE-BUILTIN was the one
// target shape whose boxability the nullability changed:
//   * `isBoxableValueType(Guid?)` was FALSE, so the `as` fell through to the
//     REFERENCE-target arm and produced a POINTER where the `Guid?` slot
//     wants the tagged `{i8 hasValue; Guid}` carrier. The store into the
//     local failed and `lowerLocalDecl` degraded the local to a zero value —
//     a SILENT wrong answer (`False` for `x <> null`), not a rejection.
//   * `boxDescriptorFor(Guid?)` resolved NO descriptor (the value-builtin's
//     descriptor is keyed by the NON-nullable name, and a builtin class sym
//     is not in `descArray_`), so the `is` test hit the #2775 "dropped" bail
//     and produced no answer at all.
//
// THE SPEC RULE. §12.7 ("A nullable target tests as its underlying type")
// and §17.4 ("`as` on value types": "When the target type is a value type,
// `x as T` produces `T?`: the boxed `Object`-or-interface input is unboxed
// to the value type if compatible, and the result is `null` otherwise"). The
// box carries the UNDERLYING type's descriptor (§8.6), so the test must too.
//
// ORACLE: exact stdout under the CHECKED (ASan/LSan) leg.
//
// BOUNDARY CONTROL (this suite's ONE base-green row). §17.4: "the result is
// `null` otherwise" — a FAILED unbox yields null, not a garbage payload. The
// row below catches THE WRONG FIX: an unbox that loads the payload without
// the `isA` guard would read the int box's bytes as a `Guid` and answer
// `True` (or crash), so this row goes RED under that fix. Every other
// base-green control of this family (the BARE `as Guid` target, the record
// twin, the primitive twin) is green with AND without the fix and its
// verdict is fixed by a spec sentence, so those are CORPUS CANDIDATES, not
// gtest rows — they live at
//   /var/tmp/bezel-scratch/r42-m36-8593/corpus-candidates/.
//
// The `int?` / `Pt?` twins of the issue's title are the admitting controls
// (a nullable primitive and a nullable record DID unbox correctly all
// along); they are corpus candidates for the reason above.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro, verbatim (#8593).
std::string guidAsNullableTarget() {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    var g: IComparable<Guid> := Guid.Empty; "
           "    var x: Guid? := g as Guid?; "
           "    Console.WriteLine(x <> null); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The `DateTime` twin of the issue's title (stdlib §2.5).
std::string dateTimeAsNullableTarget() {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    var d: IComparable<DateTime> := new DateTime(2020,1,1); "
           "    var y: DateTime? := d as DateTime?; "
           "    Console.WriteLine(y <> null); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The `is` twin: §12.7's "`x is T?` has the answer of `x is T`".
std::string guidIsNullableTarget() {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    var o: Object := Guid.Empty; "
           "    Console.WriteLine(o is Guid?); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The `as` twin over an `Object`-slot box (the §8.6 box the issue's own
// record/primitive controls use).
std::string guidAsNullableTargetOverObjectSlot() {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    var o: Object := Guid.Empty; "
           "    var x: Guid? := o as Guid?; "
           "    Console.WriteLine(x <> null); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// THE BOUNDARY CONTROL. A box that is NOT a `Guid` must unbox to null
// (§17.4: "the result is `null` otherwise"), never to a garbage payload.
std::string guidAsNullableTargetOverForeignBox() {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    var o: Object := 5; "
           "    var x: Guid? := o as Guid?; "
           "    Console.WriteLine(x <> null); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

void expectStdout(const std::string& src, const char* expected) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// The issue's own repro: a boxed `Guid` (stdlib §2.6) unboxed with the
// nullable target `Guid?` — §12.7's "`x as T?` is `x as T`".
BEZEL_TEST(codegen_boxed_value_builtin_nullable_target_as_8593,
           guid_as_nullable_target_unboxes_spec_12_7) {
    expectStdout(guidAsNullableTarget(), "True\n");
}

// The `DateTime` twin (stdlib §2.5).
BEZEL_TEST(codegen_boxed_value_builtin_nullable_target_as_8593,
           datetime_as_nullable_target_unboxes_spec_12_7) {
    expectStdout(dateTimeAsNullableTarget(), "True\n");
}

// The `is` twin: §12.7's "`x is T?` has the answer of `x is T`" — the test
// must ANSWER, not be dropped.
BEZEL_TEST(codegen_boxed_value_builtin_nullable_target_as_8593,
           guid_is_nullable_target_answers_the_underlying_type_spec_12_7) {
    expectStdout(guidIsNullableTarget(), "True\n");
}

// The `as` twin over an `Object`-slot box (the §8.6 box).
BEZEL_TEST(codegen_boxed_value_builtin_nullable_target_as_8593,
           guid_as_nullable_target_over_object_slot_spec_8_6) {
    expectStdout(guidAsNullableTargetOverObjectSlot(), "True\n");
}

// THE BOUNDARY CONTROL (the suite's one base-green row). §17.4: "the result
// is `null` otherwise". This row catches THE WRONG FIX: an unbox that loads
// the payload without the `isA` guard reads the int box's bytes as a `Guid`
// and answers `True` (or crashes) where the spec requires `null`.
BEZEL_TEST(codegen_boxed_value_builtin_nullable_target_as_8593,
           guid_as_nullable_target_over_a_foreign_box_is_null_spec_17_4) {
    expectStdout(guidAsNullableTargetOverForeignBox(), "False\n");
}
