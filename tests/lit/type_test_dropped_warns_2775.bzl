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
// is expected, the value is boxed", so a value type whose runtime identity the
// implementation cannot mint has no descriptor for `is` to compare against and
// the test is dropped. Before this fix such a program compiled silently and
// answered false.
//
// ⚠ MAINTENANCE NOTE: this test asserts the GUARD, not any particular boxing
// gap. RE-POINTED TWICE by design, exactly as the note below directs: the
// original pin was `o is [3]int` on a non-owning inline array (both `decimal`
// and inline arrays were unboxed when this landed); #2793 boxed `decimal` and
// #2804 boxed non-owning inline arrays, so the pin now sits on the category
// that remains — and durably so — runtime-ERASED element types. §12.9 / #2012
// make a function type runtime-indistinguishable from its siblings, so
// `[2](int) -> int` deliberately gets no box identity: minting one would
// collapse `[2](int) -> int` and `[2](string) -> string` onto a single
// descriptor, which is the #2001 type-confusion class. If a later change ever
// reifies function types, the CHECK stops matching — that is a correct signal,
// and the right response is to re-point the test at whatever value-type
// category remains unboxed (`unit`, §4.7 / §8.6 "Boxing of the unit value", is
// the next one), NOT to delete the assertion.
//
// The CHECK-NOT half is the accept-control: a correctly-boxed operand and a
// concrete class target must stay SILENT, so the guard cannot be satisfied by
// a predicate that simply warns on every type test. Since #2804 the non-owning
// inline array is one of those accept-controls rather than the pinned shape.
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

    // Accept-control since #2804: a NON-OWNING inline array boxes per §8.6
    // ("Non-owning inline arrays (`[N]T` and `[N]T?`) are ordinary value types
    // and box per the general rule") and the test answers correctly — silent.
    var ia: [3]int := [1, 2, 3];
    var o: Object := ia;
    if o is [3]int then else return 3; end;

    // The pinned shape: an inline array whose ELEMENT type is runtime-erased.
    // §15.1 admits function values as elements, but §12.9 / #2012 make two
    // function types runtime-indistinguishable, so no box identity may be
    // minted for `[2](int) -> int` and the test is dropped — with this
    // diagnostic rather than in silence.
    var fs: [2](int) -> int := [lambda(x: int) => x, lambda(x: int) => x + 1];
    var of: Object := fs;
    if of is [2](int) -> int then return 4; end;

    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: this type test was DROPPED and reads as false — no runtime descriptor resolved for the target type (a value type reaching an Object/interface location must be boxed, §8.6) — #2775
// CHECK-NOT: #2775
