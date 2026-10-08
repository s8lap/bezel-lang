// fix #5610 (§35.2, v0.315): `override Equals(x: int)` on a choice is not
// one of the admitted replacement members. §35.2 defers to §10.5's list,
// which names only: `Equals(E)` (the choice's own type), `Equals(Object?)`
// (open question #5598, but still admitted until ruled), `GetHashCode()`,
// and `ToString()`. A parameter type of `int` (or any type other than the
// choice's own type or `Object?`) is not admitted.
//
// REJECT: `override Equals` with wrong parameter type on choice.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; choice Color Red(payload: int), Green, Blue(msg: string); public override method Equals(x: int): bool => true; end Color; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: choice method `Equals` may not be virtual, abstract, or override — choices dispatch statically (§35.2)
