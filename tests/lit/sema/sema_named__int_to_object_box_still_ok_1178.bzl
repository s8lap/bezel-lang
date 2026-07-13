// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, int_to_object_box_still_ok_1178).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M() var o: Object := 5; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
