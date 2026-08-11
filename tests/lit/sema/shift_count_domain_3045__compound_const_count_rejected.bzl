// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §6.6 (v0.242, proposal 0079 / #3045): "`x <<= k` and `x >>= k` inherit §6.5's
// count-domain rule through the desugaring: an out-of-domain count is the same
// fault — compile error for a constant count, `OverflowException` at runtime
// otherwise." The domain is the TARGET's width (the desugared left operand), so
// the `long` spelling of the count (`40L`) is irrelevant to it.
// Before v0.242 this compiled and stored `1 << (40 mod 32)` == 256 (#1041).

module M; class C public static method Main(): int var x: int := 1; x <<= 40L; return x; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: shift count 40 is outside the domain `0..31` of the left operand's type `int`
// CHECK-SAME: (§6.5/§17.14)
// CHECK-SAME: write `x <<= (k & 31)` if the modular count is intended
