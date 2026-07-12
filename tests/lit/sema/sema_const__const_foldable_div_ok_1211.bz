// Ported from tests/sema/const_type_test.cpp
//   (BEZEL_TEST sema_const, const_foldable_div_ok_1211).
//
// §22.12 (#1211): a foldable `const` initializer using `/` (non-zero divisor)
// must still compile without errors and carry the correct folded value.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T end T; class C public const A: int := 10 / 2; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
