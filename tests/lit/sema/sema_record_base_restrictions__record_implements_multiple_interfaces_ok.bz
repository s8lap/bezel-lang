// Ported from tests/sema/record_base_restrictions_test.cpp
//   (BEZEL_TEST sema_record_base_restrictions, record_implements_multiple_interfaces_ok).
//
// (e) §9.6 CONTROL: record implementing multiple interfaces is valid.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IFoo method GetA(): int; end IFoo; interface IBar method GetB(): int; end IBar; record R(A: int, B: int) : IFoo, IBar public method GetA(): int => A; public method GetB(): int => B; end R;

// NODIAG-NOT: error
// NODIAG-NOT: §
