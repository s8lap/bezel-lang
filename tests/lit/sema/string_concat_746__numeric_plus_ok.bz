// Ported from tests/sema/string_concat_746_test.cpp
//   (BEZEL_TEST string_concat_746, numeric_plus_ok).
//
// §4.6: numeric `+` is unaffected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class F public static method Main(): int   return 3 + 4; end Main; end F;

// NODIAG-NOT: error
// NODIAG-NOT: §
