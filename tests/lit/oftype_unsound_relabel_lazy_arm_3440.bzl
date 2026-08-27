// narrow issue 3440 (stdlib §5 *Filtering*; spec §8.1, §12, §12.7; v0.283)
// — the LAZY-receiver sema arm, the second of the two.
//
// TWO independent sema arms type `OfType<U>()`: one for a concrete collection
// receiver and one for a lazy / `Range` / `IGrouping` receiver. #3440 measured
// the identity-relabel defect reproducing IDENTICALLY through both — the same
// `OwningArray<Animal>{Cat}.OfType<Dog>()` with a `Where(...)` in front still
// reported one element and still SIGSEGV'd on the `Dog`-only field.
//
// This file exists because a rejection wired into only one arm would read as a
// complete fix while half the operator's surface kept miscompiling. Its sibling
// `oftype_unsound_relabel_not_implemented_3440.bzl` pins the collection arm and
// the full wording; this one pins that the lazy arm reaches the SAME diagnostic,
// so the two cannot drift apart into two spellings of one rule.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M3440Lazy;

class Animal
  public Tag: int := 1;
end Animal;

class Cat: Animal
end Cat;

class Dog: Animal
  public DogOnly: int := 77;
end Dog;

class C
  public static method Main(): int
    using var xs := new OwningArray<Animal>();
    xs.Add(new Cat());
    var n := 0;
    for var d in xs.Where(lambda(x: Animal) => true).OfType<Dog>() do
      n := n + d.DogOnly;
    end;
    return n;
  end Main;
end C;

// Located at the call, never 0:0, and carrying the same not-implemented
// framing, the same issue pointer and the same remedy as the collection arm.
// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `OfType<Dog>()` on a sequence of `Animal`
// CHECK-SAME: is not yet implemented
// CHECK-SAME: (stdlib §5 / issue 3440)
// CHECK-SAME: needs a per-element runtime type test and the compiler emits none
// CHECK-SAME: Filter explicitly with `Where` and an `is` test (spec §12.7)
