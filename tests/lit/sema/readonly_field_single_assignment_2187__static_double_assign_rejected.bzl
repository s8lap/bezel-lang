// §22.8 (v0.226) single-assignment of readonly fields (#2187).
//
// §7.4 narrows a nullable `readonly` field "instance OR static", so a
// `static readonly` field double-assigned in the static constructor is the
// same soundness hole as the instance case. Reject the second assignment in
// the static ctor body.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static readonly _s: string?; static constructor() _s := "a"; _s := null; end constructor; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_s` more than once
// CHECK-SAME: (§22.8 single-assignment)
