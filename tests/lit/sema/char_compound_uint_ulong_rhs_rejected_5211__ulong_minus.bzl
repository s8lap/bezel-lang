// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §6.6, v0.313 (#5211): a `char` compound target's right operand is typed as in
// `int op e` under §6.2; "a non-constant `uint` or `ulong` operand has no §6.2
// promotion with `int` and is a compile error". Before the fix `c += u` with
// u = 4294967295 compiled and stored char 99 with no OverflowException.

module M; class C public static method Main(): int var c: char := char(100); var u: ulong := 1; c -= u; return int(c); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `-=` on a `char` target has no `ulong` right operand
// CHECK-SAME: (§6.6, v0.313)
