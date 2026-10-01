// Codegen — §12.7 / §8.6 / §17.4, #3120: a `x is T` on a CONCRETE
// non-nullable VALUE-typed operand (a primitive, a record, an inline array,
// the unit type, a value-builtin) must decide the test STATICALLY at the
// operand's declared type — `true` when the target is identical to the
// operand's static type, `false` otherwise — rather than being silently
// DROPPED to a constant `false` by codegen. Both `is`/`is not` and the
// `x as T` (T?) wrap / null shape are covered.
//
// spec §12.7 (v0.252), *Type tests: `is`, `is not`, `as`*:
//   "`is T` tests whether a value is an instance of `T` and produces `bool`."
//   "Both `is` and `as` are **non-destructive on their operand**: the
//    operation inspects the runtime type tag without moving from the operand."
//
// spec §8.6 (v0.252), *Boxing*:
//   "When a value type is used where `Object` (or an interface implemented by
//    the value type) is expected, the value is **boxed**."
//
// spec §17.4 (v0.252), *The `is`, `is not`, and `as` operators*:
//   "`as` on value types. When the target type is a value type, `x as T`
//    produces `T?`."
//
// THE DEFECT.
//
// A `TypeTestExpr` whose operand is statically a non-nullable value type
// lowers in `Emitter::lowerTypeTest` (LlvmBackend_Layout.cpp) to a
// non-pointer value (an `i32`, a record struct, a `[N x T]` aggregate, …).
// The pre-fix code did:
//
//     if (!obj->getType()->isPointerTy()) {
//         // nullable value type: handle hasValue
//         // reference location: reportDroppedTest + return nullptr
//         return nullptr;     // <-- the silent drop
//     }
//
// For a concrete non-nullable VALUE-typed operand (NOT nullable, NOT a
// reference location, NOT a generic-template bare type-parameter operand)
// neither prior arm matched, so `lowerIf` turned the null condition into an
// unconditional branch PAST the controlled body — a constant `false`.
//
// WHY IT WAS SILENT AT EVERY SEVERITY: `nullptr` is "recover from an
// internal limitation", not "reject this program"; the existing
// `reportDroppedTest` warning (the #2775 silence amplifier) was deliberately
// gated on `operandIsReferenceLocation()`, whose stated reason is that a
// non-`Object` operand is erased inside an UNSPECIALIZED generic template
// body (issue #490). That reasoning does not cover a CONCRETE value-typed
// operand in a live body, which is what this issue is about.
//
// MEASURED ON THE PRE-FIX COMPILER, the issue's own two shapes (one per
// concrete kind): the `int` test `if p is int then return 1; end; return 2;`
// printed 2 (should be 1); the record test `if q is Pt then return 1; end;
// return 2;` printed 2 (should be 1). The issue's "two lines" repro is the
// minimal pin; the rest of this file is the axis grid.
//
// THE FIX. A new static-decision branch in `lowerTypeTest`, gated on a new
// `concreteValueTypeOperand` predicate that admits primitive (non-string/
// non-Object), record, enum, value-builtin, inline array (non-nullable),
// and unit — and EXCLUDES the nullable value type (the existing hasValue
// arm handles it), the reference location (existing reportDroppedTest arm
// handles it), the function type (reference type, lowers to ptr, never
// reaches the non-pointer branch), and the bare type-parameter operand
// (Named with declaration == nullptr — the #490 unspecialized body that is
// never executed, where `reportDroppedTest` deliberately stays silent so
// dead code never warns). The decision reads `staticTypeIdentical(operand,
// target)`: identical → true, else → false.
//
// Both predicates live in LlvmBackend_impl.hpp next to the existing
// `isObjectOrInterfaceTarget` / `isBoxableValueType` helpers (no new file).
// `nullableValueStructTy(rt)` / `nullableValueNull(rt)` (the same helpers
// the nullable value-type arm uses) provide the T? wrap / null shape for
// `as`. The generic-template shape (`o is T` in `Box<T>`) keeps its
// pre-#3120 silent-drop; that path is filed separately and is unchanged by
// this fix.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

int run(const char* src) {
    JitResult r = runAot(src);
    if (!r.ran) return -1;
    return r.exitCode;
}

// =====================================================================
// The two shape the issue prints verbatim.
// =====================================================================

// §12.7 — primitive operand. Pre-fix: 2. Want 1.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           primitive_int_operand_spec_12_7_8_6_17_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is int then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// §12.7 — record operand. Pre-fix: 2. Want 1.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           record_operand_spec_12_7_8_6_17_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var q: Pt := new Pt(7, 9); "
        "    if q is Pt then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// =====================================================================
// Axis grid — every concrete value-kind predicate admits, decided
// statically. Pre-fix: every shape printed 2. Want 1.
// =====================================================================

// bit 1 long, 2 double, 4 bool, 8 char, 16 decimal. Want 31.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           numeric_primitive_operands_spec_12_7_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var code := 0; "
        "    var a: long := 1L;     if a is long   then code := code + 1;  end; "
        "    var b: double := 1.0;  if b is double then code := code + 2;  end; "
        "    var c: bool := true;   if c is bool   then code := code + 4;  end; "
        "    var d: char := 'x';    if d is char   then code := code + 8;  end; "
        "    var e: decimal := 1m;  if e is decimal then code := code + 16; end; "
        "    return code; "
        "  end Main; "
        "end C;"), 31);
}

// §4.7 — the unit type. Pre-fix: 2. Want 1.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           unit_operand_spec_12_7_8_6_4_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var u: () := (); "
        "    if u is () then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// §15.1 — non-nullable inline array operand. Pre-fix: 2. Want 1.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           inline_array_operand_spec_12_7_8_6_15_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: [3]int := [1, 2, 3]; "
        "    if a is [3]int then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// §10 — enum operand (the enum's underlying integer). Pre-fix: 2. Want 1.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           enum_operand_spec_12_7_8_6_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Col Red, Green, Blue; end Col; "
        "class C "
        "  public static method Main(): int "
        "    var c: Col := Col.Green; "
        "    if c is Col then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// =====================================================================
// Flips per #5175 (§12.7, v0.310/#5157): concrete value typed operand
// against a different value type is now a compile error. The old admit rows
// for non-matching targets become reject rows. Same-type admit stays.
// =====================================================================

// §12.7 — same type: `int is int` still admits. Want 1.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           matching_target_on_primitive_still_admits_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is int then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// §12.7 (v0.310/#5157) — flip: `int is long` was admit, now reject.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           int_is_long_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is long then return 99; end; "
        "    return 3; "
        "  end Main; "
        "end C;").diagnostics.hasErrors());
}

// §12.7 (v0.310/#5157) — flip: `int is double` was admit, now reject.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           int_is_double_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is double then return 99; end; "
        "    return 3; "
        "  end Main; "
        "end C;").diagnostics.hasErrors());
}

// §12.7 (v0.310/#5157) — flip: `int is Pt` was admit, now reject.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           int_is_record_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(runAot(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is Pt then return 99; end; "
        "    return 3; "
        "  end Main; "
        "end C;").diagnostics.hasErrors());
}

// §12.7 (v0.310/#5157) — flip: `[3]int is [4]int` was admit, now reject.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           inline_array_size_mismatch_rejected_spec_12_7_15_1) {
    BEZEL_EXPECT_TRUE(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: [3]int := [1, 2, 3]; "
        "    if a is [4]int then return 99; end; "
        "    return 1; "
        "  end Main; "
        "end C;").diagnostics.hasErrors());
}

// §12.7 — same inline-array type: `[3]int is [3]int` still admits. Want 1.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           matching_inline_array_target_still_admits_spec_12_7_15_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: [3]int := [1, 2, 3]; "
        "    if a is [3]int then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// §12.7 (v0.310/#5157) — flip: `a is B` was admit, now reject.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           record_type_mismatch_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(runAot(
        "module M; "
        "record A(X: int); "
        "record B(X: int); "
        "class C "
        "  public static method Main(): int "
        "    var a: A := new A(7); "
        "    if a is B then return 99; end; "
        "    return 1; "
        "  end Main; "
        "end C;").diagnostics.hasErrors());
}

// §12.7 — same record type: `a is A` still admits. Want 1.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           matching_record_target_still_admits_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record A(X: int); "
        "class C "
        "  public static method Main(): int "
        "    var a: A := new A(7); "
        "    if a is A then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// =====================================================================
// `is not` and the `as` shape (T? wrap / null). Pre-fix: every shape
// either silently dropped or produced malformed IR. Want the spec-stated
// values.
// =====================================================================

// §12.7 (v0.310/#5157) — flip: `p is not long` was admit (returns 2),
// now reject (different concrete value type).
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           int_is_not_long_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    var code := 0; "
        "    if p is not int  then code := code + 100; end; "
        "    if p is not long then code := code + 2;   end; "
        "    return code; "
        "  end Main; "
        "end C;").diagnostics.hasErrors());
}

// §12.7 — `p is not int` still admits (same type, returns false). Want 2.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           is_not_same_type_still_admits_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    if p is not int then return 99; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 2);
}

// §17.4 — `x as T` on a value-typed operand where target == operand's
// static type produces T? wrapping the operand. Want 1.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           as_match_wraps_value_spec_17_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    var q: int? := p as int; "
        "    if q <> null and q! = 7 then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// §17.4 (v0.310/#5157) — flip: `p as long` where p is int was admit
// (produces null), now reject (different concrete value types).
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           as_value_type_mismatch_rejected_spec_17_4) {
    BEZEL_EXPECT_TRUE(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var p: int := 7; "
        "    var q: long? := p as long; "
        "    if q = null then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;").diagnostics.hasErrors());
}

// =====================================================================
// OVER-REACH CONTROLS. The static decision fires ONLY on a concrete
// non-nullable value-typed operand. The neighbouring shapes must stay
// byte-identical to their pre-fix behaviour, so a fix that is too wide
// is a worse defect than the one it closes.
// =====================================================================

// §12.7 / §8.6 — a NULLABLE value type operand stays on the hasValue
// arm; the `concreteValueTypeOperand` predicate excludes nullable. Want 6
// (present) and 2 (absent) — exactly the pre-fix behaviour.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           nullable_value_operand_still_uses_hasvalue_spec_7_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int? := 5; "
        "    if n is int then return n! + 1; end; "
        "    return 99; "
        "  end Main; "
        "end C;"), 6);
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int? := null; "
        "    if n is int then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 2);
}

// §12.7 / §8.6 — an `Object` operand (the issue's `#3090` neighbour) is
// a REFERENCE location; the static-decision branch's
// `concreteValueTypeOperand` excludes it, so the existing boxed-value path
// still answers correctly. Want 1.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           reference_location_operand_unchanged_spec_8_6_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    if o is int then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// §12.7 — a CLASS narrowing at an `Object` location stays on the
// descriptor-walk arm; the predicate excludes a class operand's static
// type. Want 7.
BEZEL_TEST(codegen_type_test_value_typed_operand_3120,
           class_narrowing_unchanged_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal "
        "  public constructor() end constructor; "
        "end Animal; "
        "class Dog: Animal "
        "  public constructor() base(); end constructor; "
        "  public method Bark(): int => 7; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    var o: Object := d; "
        "    if o is Dog then return o.Bark(); end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 7);
}

}  // namespace