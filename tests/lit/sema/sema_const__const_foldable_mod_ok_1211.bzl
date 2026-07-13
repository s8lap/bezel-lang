// Ported from tests/sema/const_type_test.cpp
//   (BEZEL_TEST sema_const, const_foldable_mod_ok_1211).
//
// §22.12 (#1211): a foldable `const` initializer using `mod` (non-zero
// divisor) must compile without errors — this also validates that `mod` is now
// properly handled by the const folder (it was silently ignored before).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T end T; class C public const B: int := 7 mod 3; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
