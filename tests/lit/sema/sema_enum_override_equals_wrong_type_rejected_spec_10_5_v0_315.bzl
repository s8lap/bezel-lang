// fix #5610 (§10.5, v0.315): `override Equals(x: int)` on an enum is not
// one of the admitted replacement members. §10.5 names only: `Equals(E)`
// (the enum's own type), `Equals(Object?)` (open question #5598, but still
// admitted until ruled), `GetHashCode()`, and `ToString()`. A parameter
// type of `int` (or any type other than the enum's own type or `Object?`)
// is not admitted.
//
// REJECT: `override Equals` with wrong parameter type on enum.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; enum Color Red, Green, Blue; public override method Equals(x: int): bool => x >= 0; end Color; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum method `Equals` may not be virtual, abstract, or override — enums dispatch statically (§10.5)
