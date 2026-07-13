// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, instance_member_in_normal_class_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Normal public _x: int := 0; public method Inst() _ := 1; end Inst; end Normal;

// NODIAG-NOT: error
// NODIAG-NOT: §
