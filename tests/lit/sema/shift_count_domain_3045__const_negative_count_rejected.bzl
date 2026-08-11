// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §6.5 (v0.242, proposal 0079 / #3045): "The constant-count diagnostic for a
// negative count notes the direction confusion: a negative count does not
// reverse the shift's direction — use the opposite operator, or mask
// deliberately with `& 31`."
// Before v0.242 this compiled: the implicit `& 31` turned -1 into 31, so
// `1 << -1` silently produced int.MinValue.

module M; class C public static method Main(): int var v: int := 1; var r := v << -1; return r; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: shift count -1 is outside the domain `0..31` of the left operand's type `int`
// CHECK-SAME: (§6.5/§17.14)
// CHECK-SAME: a negative count does not reverse the shift's direction — use the opposite operator (`>>`), or mask deliberately with `(k & 31)`
