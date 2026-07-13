// Ported from tests/sema/class_modifier_combos_test.cpp
//   (BEZEL_TEST sema_class_mods, member_plain_virtual_ok_r39).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class B public virtual method F(): int return 1; end F; end B;

// NODIAG-NOT: error
// NODIAG-NOT: §
