// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, out_arg_invalidates_null_narrowing_1188).
//
// §7.4 (#1188): an `out` argument is likewise (re)written by the callee, so it
// invalidates the passed-in local's null-narrowing.

module M; class User public Name: int := 0; end User; class C public static method Reset(out u: User?) u := null; end Reset; public static method Run(): int var u: User? := new User(); if u <> null then Reset(out u); return u.Name; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Name` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
