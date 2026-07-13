// Ported from tests/sema/generic_default_new_constraint_864_test.cpp
//   (BEZEL_TEST generic_default_new_constraint_864, part2_parameterless_ctor_accepted).
//
// §14.2 (#864 part 2 ACCEPT): `Make<Foo>()` where Foo has a parameterless
// constructor MUST be accepted — Foo satisfies `where T: new()`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo public constructor() end constructor; destructor end destructor; end Foo; static class Utils public static method Make<T>(): int where T: class, new() using var t := new T(); return 1; end Make; end Utils; class C public static method Main(): int return Utils.Make<Foo>(); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
