// fix #4657 (§3.1 / §8.1 / §14.4, v0.315): `owningElementBadDesc` (TypeResolution_Checks.cpp)
// classified `PrimitiveType::Object` as "a primitive value type", rejecting every owning
// collection instantiated at `Object` with a misdescriptive diagnostic.
//
// THE FIX. owningElementBadDesc now returns {} (admitted) for PrimitiveType::Object:
// Object is the root class (§3.1 / §8.1), has a destructor, implements IDisposable,
// and satisfies `where T: class`.
//
// THE SPEC. §14.4: "Most standard-library owning types … restrict their element type T
// to class and interface types." Object is a class, so OwningSet<Object> is legal.
//
// Regression: owningset_object_admitted_4657
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int using var s := new OwningSet<Object>(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
