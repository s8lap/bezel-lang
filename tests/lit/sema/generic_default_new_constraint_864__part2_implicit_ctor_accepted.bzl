// Ported from tests/sema/generic_default_new_constraint_864_test.cpp
//   (BEZEL_TEST generic_default_new_constraint_864, part2_implicit_ctor_accepted).
//
// §14.2 (#864 part 2 ACCEPT): `Make<NoCtorClass>()` where NoCtorClass has
// NO explicit constructor (implicit default ctor) must be accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class NoCtorClass public n: int := 0; destructor end destructor; end NoCtorClass; static class Utils public static method Make<T>(): int where T: class, new() using var t := new T(); return 1; end Make; end Utils; class C public static method Main(): int return Utils.Make<NoCtorClass>(); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
