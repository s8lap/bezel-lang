// Ported from tests/sema/record_base_restrictions_test.cpp
//   (BEZEL_TEST sema_record_base_restrictions, record_implements_interface_ok).
//
// (d) §9.6 CONTROL: record implementing a single interface is valid.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IFoo method GetVal(): int; end IFoo; record R(Val: int) : IFoo public method GetVal(): int => Val; end R;

// NODIAG-NOT: error
// NODIAG-NOT: §
