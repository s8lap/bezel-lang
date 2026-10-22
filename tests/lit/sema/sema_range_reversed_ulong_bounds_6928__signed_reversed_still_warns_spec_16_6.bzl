// #6928 (§16.6, v0.340): a genuinely reversed signed range `5..1` over an int
// scrutinee still warns. The fix changes only the ulong readback.
//
// Control: this row passes on the pre-fix compiler too.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method M(x: int): int return (case x of 5..1 => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} this pattern never matches — bounds are reversed
// CHECK-SAME: (§16.6)
