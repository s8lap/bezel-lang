// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §6.5 (v0.242, proposal 0079 / #3045): "The shift count's domain is `0` through
// `width−1` … A count outside the domain — negative, or ≥ width — is a fault: a
// compile error when the count is a compile-time constant (§17.14)".
// `int`'s domain is `0..31`; 32 is the first count outside it.
// Before v0.242 this compiled and silently yielded `1 << (32 mod 32)` == 1.

module M; class C public static method Main(): int var v: int := 1; var r := v << 32; return r; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: shift count 32 is outside the domain `0..31` of the left operand's type `int`
// CHECK-SAME: a compile-time-constant count outside the domain is a compile error (§6.5/§17.14)
// CHECK-SAME: write `x << (k & 31)` if the modular count is intended
