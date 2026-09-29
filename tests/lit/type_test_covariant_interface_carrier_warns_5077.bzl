// fix #5077 (narrow revision of #4926; codegen, LlvmBackend_Layout.cpp
// lowerTypeTest, spec §12.7 / §12.9 / §14, v0.307): #4926's Case B originally
// folded the COVARIANT collection-element interfaces (`IEnumerable<out T>`,
// `IReadOnlyCollection<out T>`, `IReadOnlyList<out T>` — stdlib declares all
// three `out T`) into the same "argument mismatch -> false" decision it used
// for the invariant ones. That is UNSOUND: §12.9's own worked example is
// exactly this shape ("`IEnumerable<Dog>` is assignable to
// `IEnumerable<Animal>`"), so `v: IEnumerable<Animal> := (an Array<Dog>);
// v is Array<Dog>` has an "Animal" vs "Dog" argument MISMATCH yet the
// correct answer is TRUE. Reverting the #5077 fix (i.e. re-widening Case B
// to include these three names again, as `5f2c8753` did) makes all three
// rows below answer a confident, undiagnosed `false` instead of warning —
// worse than the pre-#4926 state, because it pre-empts the #2775 "dropped"
// warning the widened `concreteTarget` bail below would otherwise emit for
// this still-genuinely-ambiguous shape. Deciding it soundly needs a runtime
// collection-kind tag that does not exist yet (#5059); no case here is
// "trivially sound true" — a covariant carrier's mismatched argument proves
// nothing in either direction, matched or not, without one.
//
// This pins that all three covariant carriers fall through to the #2775
// warning instead of Case B's confident false. The three accepted-assignment
// shapes themselves (`Array<Dog>` -> `IEnumerable<Animal>` /
// `IReadOnlyCollection<Animal>` / `IReadOnlyList<Animal>`) are already pinned
// clean-compile in
// tests/lit/sema/sema_variance_assign__spec_12_9_ienumerable_animal_from_array_dog_accepted_1180.bzl
// and the `IReadOnlyList` sibling; this file is the type-TEST half.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;

class Animal end Animal;
class Dog : Animal end Dog;

class C
  public static method Main(): int
    using var d1 := new Dog();
    var xs: Array<Dog> := [d1];

    var v1: IEnumerable<Animal> := xs;
    if v1 is Array<Dog> then Console.WriteLine("v1-true");
    else Console.WriteLine("v1-false"); end;

    var v2: IReadOnlyCollection<Animal> := xs;
    if v2 is Array<Dog> then Console.WriteLine("v2-true");
    else Console.WriteLine("v2-false"); end;

    var v3: IReadOnlyList<Animal> := xs;
    if v3 is Array<Dog> then Console.WriteLine("v3-true");
    else Console.WriteLine("v3-false"); end;

    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: this type test was DROPPED and reads as false — no runtime descriptor resolved for the target type (a value type reaching an Object/interface location must be boxed, §8.6) — #2775
// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: this type test was DROPPED and reads as false — no runtime descriptor resolved for the target type (a value type reaching an Object/interface location must be boxed, §8.6) — #2775
// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: this type test was DROPPED and reads as false — no runtime descriptor resolved for the target type (a value type reaching an Object/interface location must be boxed, §8.6) — #2775
// CHECK-NOT: #2775
