// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, in_range_and_auto_increment_ok).
//
// §10.3 regression guard: in-range values, an unsigned boundary, a signed
// minimum, and AUTO-INCREMENT continuation are all accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum B: byte Lo := 0, Mid := 200, Hi := 255; end B; enum S: short A := -32768, Z := 32767; end S; enum Inc: byte X := 253, Y, Z; end Inc;

// NODIAG-NOT: error
// NODIAG-NOT: §
