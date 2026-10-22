// #6928 (§16.6, v0.340): `18446744073709551615UL..5UL` is reversed as ulong
// values (the lower bound is the ulong maximum, above 5), so it never matches
// and must warn. The pre-fix compiler folds the lower bound to -1 and misses the
// warning, so this row fails on the pre-fix compiler.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method M(): int var u: ulong := 3; return (case u of 18446744073709551615UL..5UL => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} this pattern never matches — bounds are reversed
// CHECK-SAME: (§16.6)
