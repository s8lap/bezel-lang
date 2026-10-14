// fix #5633 (§35.2, v0.334): `override Equals(x: Object)` (non-nullable) on
// a choice is not one of the admitted replacement members. §35.2 defers to
// §10.5's list, which names only: `Equals(E)` (the choice's own type),
// `Equals(Object?)` (admitted per §9.2 / #5598), `GetHashCode()`, and
// `ToString()`. A parameter type of `Object` (non-nullable) is not admitted;
// only `Object?` is.
//
// Symmetric to the enum case: the earlier arity-1 check did not reject
// `Equals(Object)` (only wrong types like `int`); b530bf05 narrowed the arm
// to also reject non-nullable Object, and this row pins that distinction.
//
// REJECT: `override Equals` with non-nullable Object parameter on choice.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; choice Color Red(payload: int), Green, Blue(msg: string); public override method Equals(x: Object): bool => true; public override method GetHashCode(): int => 1; end Color; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: choice method `Equals` may not take `Object` — a choice's `Object` slot is filled by its own-typed `Equals(other: Color)`, spelled `override` or plain (§35.2)
