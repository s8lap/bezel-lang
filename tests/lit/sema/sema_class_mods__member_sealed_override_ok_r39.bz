// Ported from tests/sema/class_modifier_combos_test.cpp
//   (BEZEL_TEST sema_class_mods, member_sealed_override_ok_r39).
//
// Valid combinations still compile.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class B public virtual method F(): int return 1; end F; end B; class D: B public sealed override method F(): int return 2; end F; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
