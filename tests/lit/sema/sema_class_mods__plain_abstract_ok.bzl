// Ported from tests/sema/class_modifier_combos_test.cpp
//   (BEZEL_TEST sema_class_mods, plain_abstract_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; abstract class Shape end Shape;

// NODIAG-NOT: error
// NODIAG-NOT: §
