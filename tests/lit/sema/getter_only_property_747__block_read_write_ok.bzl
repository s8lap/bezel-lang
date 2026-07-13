// Ported from tests/sema/getter_only_property_747_test.cpp
//   (BEZEL_TEST getter_only_property_747, block_read_write_ok).
//
// §23.3: a block property WITH a setter is assignable — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _x: int := 0; public property Val: int get => _x; set => do _x := value; end; end; public static method Main(): int using var c := new C(); c.Val := 42; return c.Val; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
