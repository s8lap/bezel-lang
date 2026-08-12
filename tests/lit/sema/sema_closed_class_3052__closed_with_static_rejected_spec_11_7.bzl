// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §11.7 (v0.242, #3052): "combining `closed` with `abstract`, `sealed`, or
// `static` is a compile error." A static class has no instances and no
// hierarchy at all, so the closed-hierarchy modifier has nothing to close.

module M; closed static class Utils public static method Max(a: int, b: int): int => (if a > b then a else b); end Utils;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Utils` may not be both `closed` and `static`
// CHECK-SAME: (§11.7)
