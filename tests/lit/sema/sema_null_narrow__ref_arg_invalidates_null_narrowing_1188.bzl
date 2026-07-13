// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, ref_arg_invalidates_null_narrowing_1188).
//
// §7.4 (#1188): a `ref` argument can be reassigned by the callee (including to
// null), so a prior null-narrowing on the passed-in local no longer holds after
// the call — a subsequent bare member access becomes an error again, exactly
// like a direct reassignment. (Before the fix this compiled and dereferenced
// the nulled value at run time → SIGSEGV.)

module M; class User public Name: int := 0; end User; class C public static method Clear(ref u: User?) u := null; end Clear; public static method Run(): int var u: User? := new User(); if u <> null then Clear(ref u); return u.Name; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Name` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
