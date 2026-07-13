// Ported from tests/sema/readonly_auto_property_747_test.cpp
//   (BEZEL_TEST readonly_auto_property_747, non_readonly_property_unqualified_write_ok_1173).
//
// §23.1 (#1173): an unqualified write to a NON-readonly auto-property inside a
// regular method must still be accepted (the new branch must not over-reject).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public property X: int := 5; public constructor() end constructor; public method Bump() X := 9; end Bump; public static method Main(): int return 0; end Main; end A;

// NODIAG-NOT: error
// NODIAG-NOT: §
