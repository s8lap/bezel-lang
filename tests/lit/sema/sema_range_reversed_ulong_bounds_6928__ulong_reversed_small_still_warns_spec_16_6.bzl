// #6928 (§16.6, v0.340): a genuinely reversed ulong range `10UL..5UL` still
// warns that it never matches. The fix must not silence reversed ulong bounds.
//
// Control: this row passes on the pre-fix compiler too.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method M(): int var u: ulong := 3; return (case u of 10UL..5UL => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} this pattern never matches — bounds are reversed
// CHECK-SAME: (§16.6)
