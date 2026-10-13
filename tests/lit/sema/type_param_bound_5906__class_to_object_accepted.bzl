// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// #5906 / §14.2 *Type-parameter bounds* (v0.338) anti-over-reject control:
// at `U := Object` the bound admits every class type, so `Rel<Dog, Object>`
// compiles clean.

module M; class Dog end Dog; class Rel<T, U> where T: U public item: T; end Rel; class C public static method Main(): int var r: Rel<Dog, Object>? := null; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
