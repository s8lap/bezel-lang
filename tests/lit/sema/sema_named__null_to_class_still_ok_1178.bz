// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, null_to_class_still_ok_1178).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Dog end Dog; class C public method M() var d: Dog? := null; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
