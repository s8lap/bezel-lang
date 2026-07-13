// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, generic_param_concrete_assign_still_ok_1178).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class U public static method Id<T>(x: T): T var y: T := x; return y; end Id; end U; class C public method M() var n: int := U.Id<int>(7); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
