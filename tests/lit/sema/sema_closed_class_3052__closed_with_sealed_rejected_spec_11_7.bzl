// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §11.7 (v0.242, #3052): "combining `closed` with `abstract`, `sealed`, or
// `static` is a compile error." `closed sealed` is the contradiction — a
// `closed` class is inheritable within its declaring module, which is exactly
// what `sealed` forbids.

module M; closed sealed class Shape end Shape;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Shape` may not be both `closed` and `sealed`
// CHECK-SAME: (§11.7)
