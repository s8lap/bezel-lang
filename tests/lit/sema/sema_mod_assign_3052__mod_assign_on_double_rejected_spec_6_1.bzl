// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §6.1 (v0.242, #3052), verbatim: "`mod` is defined on integer types and on
// `decimal` … It is **not** defined on `float` or `double`; an expression of
// the form `x mod y` where either operand has a binary floating-point type is
// a compile error." §6.6's desugaring carries the restriction to `mod=`; the
// compound forms do not route through the binary typing path, so the target
// kind is guarded on its own — exactly as §6.5's bitwise/shift compounds are.

module M; class C public static method Main(): int var d: double := 7.5; d mod= 2.0; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `mod=` is not valid on binary floating-point types
// CHECK-SAME: (§6.1)
