// Ported from tests/sema/readonly_field_assignment_747_test.cpp
//   (BEZEL_TEST readonly_field_assignment_747, in_constructor_ok).
//
// §22.8: assignment to a readonly field inside the constructor is allowed.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _v: int; public constructor() this._v := 7; end constructor; public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
