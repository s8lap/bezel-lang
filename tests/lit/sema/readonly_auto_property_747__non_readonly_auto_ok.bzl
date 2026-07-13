// Ported from tests/sema/readonly_auto_property_747_test.cpp
//   (BEZEL_TEST readonly_auto_property_747, non_readonly_auto_ok).
//
// §23.1: a non-readonly auto-property is freely assignable — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public property Version: int; public static method Main(): int using var c := new C(); c.Version := 8; return c.Version; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
