// Ported from tests/sema/getter_only_property_747_test.cpp
//   (BEZEL_TEST getter_only_property_747, set_only_plain_write_ok_1192).
//
// §23 (#1192): a plain `:=` write to a set-only property is NOT a read — it is
// dispatched to the setter and must be accepted (no write-only error).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _x: int := 0; public property Val: int set => do _x := value; end; end; public method Show(): int => _x; public static method Main(): int using var c := new C(); c.Val := 7; return c.Show(); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
