// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=SCOPE --allow-empty %s
//
// The SCOPE oracle for fix #4760 (spec §5.1 / §4.6, v0.307): the new
// "built-in heap reference into a concrete primitive" rejection must NOT fire
// on a bare TYPE VARIABLE.
//
// A declaration-less, single-part Named TypeRef is one of exactly two things:
// a reserved built-in, or a type PARAMETER (`isReservedBuiltinTypeName`'s own
// header states the dichotomy). The #4760 gate therefore reads the shared
// built-in name table AND excludes a type variable explicitly, exactly as
// #4644's built-in-exception arm does — otherwise a generic parameter whose
// name happens to SHADOW a built-in heap reference would be rejected with a
// diagnostic reasoning about a built-in that is not there.
//
// `Box<Array>` below names its parameter `Array`, so `v: Array` resolves to a
// Named TypeRef with a null declaration and the head `Array` — indistinguishable
// from `Array<T>` by name alone. The instantiation is `Box<int>`.
//
// The check is deliberately a CHECK-NOT on the rule's own wording rather than
// an exit-code assertion: whether a bare type variable may reach a primitive
// target at all is a SEPARATE, still-open question (the generic-deferral hole —
// `checkAssignable`'s free-type-var fallback accepts it today), and this file
// must keep passing whichever way that is settled. What it pins is only that
// #4760's rule is not the thing that answers it.
//
// SCOPE-NOT: is a heap reference and the target is a value type
// SCOPE-NOT: the built-in type `Array`

module BC4760;

public class Box<Array>
  public method Pick(v: Array): int
    // `v` is the TYPE PARAMETER named `Array`, not the §4.5 collection.
    var t: int := v;
    return t;
  end Pick;
end Box;

public class Program
  public static method Main(): int
    using var b := new Box<int>();
    Console.WriteLine(b.Pick(42));
    return 0;
  end Main;
end Program;
