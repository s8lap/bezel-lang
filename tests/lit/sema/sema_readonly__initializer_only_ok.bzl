// Ported from tests/sema/readonly_field_test.cpp
//   (BEZEL_TEST sema_readonly, initializer_only_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _x: int := 42; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
