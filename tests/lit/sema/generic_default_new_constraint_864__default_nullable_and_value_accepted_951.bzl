// Ported from tests/sema/generic_default_new_constraint_864_test.cpp
//   (BEZEL_TEST generic_default_new_constraint_864, default_nullable_and_value_accepted_951).
//
// ACCEPT: default(T?) of the nullable counterpart, and value-type defaults.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell destructor end destructor; end Cell; class C public static method Main(): int var c: Cell? := default(Cell?); var s: string? := default(string?); var i: int := default(int); var b: bool := default(bool); return i + (if b then 1 else 0); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
