// Ported from tests/sema/readonly_auto_property_747_test.cpp
//   (BEZEL_TEST readonly_auto_property_747, readonly_property_ctor_write_ok_1173).
//
// §23.1 (#1173): an unqualified write to a readonly auto-property INSIDE the
// constructor remains allowed (field-initializer-or-constructor context).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public readonly property X: int := 5; public constructor() X := 7; end constructor; public static method Main(): int return 0; end Main; end A;

// NODIAG-NOT: error
// NODIAG-NOT: §
