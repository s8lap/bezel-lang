// fix #2775 (codegen, LlvmBackend_Layout.cpp lowerTypeTest / LlvmBackend_Stmts.cpp
// lowerIf, spec §8.6 / §12.7 / §19.3, v0.240): the SILENCE AMPLIFIER guard.
//
// `lowerTypeTest` has four `return nullptr` bails. None of them reported a
// diagnostic, and `lowerIf` turns a null condition into an unconditional branch
// PAST the body — so a dropped type test compiled to a constant `false` in a
// program with no diagnostic at all. Two shipped defects (#2703's
// collection-literal arm and #2761) each had exactly that as their worst
// failure mode. The fix reports a Warning from every bail, through ONE shared
// definition rather than a guard copied per site.
//
// The shape pinned here is measured, not hypothetical. §8.6 says "When a value
// type is used where `Object` (or an interface implemented by the value type)
// is expected, the value is boxed", and its "Boxing of inline arrays" paragraph
// says "Non-owning inline arrays (`[N]T` and `[N]T?`) are ordinary value types
// and box per the general rule". `isBoxableValueType` does not box them (its
// own closing comment says "inline arrays / unit: deferred"), so `o is [3]int`
// below has no runtime descriptor to compare against and the test is dropped.
// Before this fix the program compiled silently and answered false.
//
// ⚠ MAINTENANCE NOTE: this test asserts the GUARD, not the boxing gap. If a
// later change makes inline arrays box per §8.6, the CHECK stops matching —
// that is a correct signal, and the right response is to re-point the test at
// whatever value-type category remains unboxed (`decimal` was the other live
// one when this landed), NOT to delete the assertion.
//
// The CHECK-NOT half is the accept-control: a correctly-boxed operand and a
// concrete class target must stay SILENT, so the guard cannot be satisfied by
// a predicate that simply warns on every type test.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;

class Widget
  public constructor() end constructor;
end Widget;

class C
  public static method Main(): int
    // Accept-control: `int` boxes per §8.6 and the test answers correctly.
    var boxed: Object := 7;
    if boxed is int then else return 1; end;

    // Accept-control: a class reference reaching Object keeps its bare pointer
    // and the descriptor walk resolves.
    using var w := new Widget();
    var asObj: Object := w;
    if asObj is Widget then else return 2; end;

    // The pinned shape: a non-owning inline array in an Object location.
    var ia: [3]int := [1, 2, 3];
    var o: Object := ia;
    if o is [3]int then return 3; end;

    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: internal: this type test was DROPPED and reads as false — no runtime descriptor resolved for the target type (a value type reaching an Object/interface location must be boxed, §8.6) — #2775
// CHECK-NOT: #2775
