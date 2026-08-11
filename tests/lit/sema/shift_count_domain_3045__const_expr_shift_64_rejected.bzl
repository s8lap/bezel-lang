// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §17.14 (v0.242, proposal 0079 / #3045), its own worked example: "An evaluation
// fault — integer or `decimal` overflow, division or `mod` by zero, a shift
// count outside the §6.5 domain — is a compile error at the constant
// expression, never a deferred runtime fault: `const B := 2147483647 + 1;`,
// `const D := 5 / 0;`, and `const F := 1 << 64;` are rejected where they are
// written". The spec's own comment on the example names the reason: "compile
// error: shift count outside 0..31, the int left operand's domain (§6.5)" — the
// literal `1` is an `int`, so 64 is outside `0..31`, not merely at `long`'s edge.
// Before v0.242 the fold masked (64 & 31 == 0) and `F` was 1.

module M; class C public const F: long := 1 << 64; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: shift count 64 is outside the domain `0..31` of the left operand's type `int`
// CHECK-SAME: (§6.5/§17.14)
