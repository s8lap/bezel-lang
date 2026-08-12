// fix #2911 + fix #2962 — spec §17.4 / §3.6 rule 2 (v0.241), the
// PER-INSTANTIATION arm. §17.4: "When the target is a generic type parameter,
// the check applies **per instantiation** (§3.6 rule 2): `x as T` at an
// inline-array or unit substitution is rejected at the instantiation site."
//
// This pins the third of the fix's three messages — the one that has to explain
// something the other two do not: WHY a program whose generic body is perfectly
// legal is nonetheless rejected. §3.6 rule 4 requires a property-dependent
// diagnostic to name "the property, the offending type argument, and the
// structural reason"; the message therefore names the parameter, the declaring
// generic, the LINE the body's `as` sits on, and then the shared §17.4 reason
// verbatim — one predicate, so the wording cannot drift from the direct arm's.
//
// Both halves of the generic reach are exercised in one file: `Box<T>`'s TYPE
// parameter (resolved by Resolver::checkAsTargetInstantiations against the
// enclosing generic's instantiations) and `Probe<T>`'s METHOD parameter, bound
// by ARGUMENT INFERENCE (resolved by ExprTC::checkDeferredAsTargetInstantiations
// against the call sites). Inference is not a stylistic choice: an inline array
// does not parse in a call's explicit type-argument list, so `Probe<[3]int>(o)`
// is unreachable and inference is the only way to bind T here.
//
// ⚠ At `Box<[3]int>` the pre-fix compiler SEGFAULTED (exit 139, core dumped)
// rather than miscompiling. If this test ever starts crashing instead of
// failing a CHECK, that is the regression it is guarding.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class Box<T>
  public constructor() end constructor;
  public method Probe(o: Object): int
    var r := o as T;
    if r <> null then return 1; end;
    return 0;
  end Probe;
end Box;

class C
  public static method Probe<T>(o: Object, witness: T): int
    var r := o as T;
    if r <> null then return 1; end;
    return 0;
  end Probe;

  public static method Main(): int
    var a: [3]int := [4, 5, 6];
    var o: Object := a;
    using var b := new Box<[3]int>();
    return b.Probe(o) + C.Probe(o, a);
  end Main;
end C;

// CHECK-DAG: type parameter `T` of `Box` is used as an `as` target in the generic body (line {{[0-9]+}}), so this instantiation is rejected at the instantiation site (§3.6 rule 2): `as` requires its result type `T?` to exist and to be informative (§17.4), and the inline-array target `[3]int` has no such result
// CHECK-DAG: type parameter `T` is used as an `as` target in the generic body (line {{[0-9]+}}), so this instantiation is rejected at the instantiation site (§3.6 rule 2): `as` requires its result type `T?` to exist and to be informative (§17.4), and the inline-array target `[3]int` has no such result
