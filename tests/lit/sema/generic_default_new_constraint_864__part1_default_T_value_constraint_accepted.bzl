// Ported from tests/sema/generic_default_new_constraint_864_test.cpp
//   (BEZEL_TEST generic_default_new_constraint_864, part1_default_T_value_constraint_accepted).
//
// §17.11 (#864 part 1 ACCEPT): `default(T) where T: value` is accepted —
// value types always have a well-defined zero value.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class Utils public static method GetZero<T>(): T where T: value return default(T); end GetZero; end Utils; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
