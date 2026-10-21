// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #6916 (§19.3, §5.1, v0.340): a ulong constant above long max over a long scrutinee (matched -1 before).
// Reported repro shape: `var u: long := -1; case u of 18446744073709551615UL`.

module M; class C public static method M(): int var u: long := -1; return (case u of 18446744073709551615UL => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `case` pattern constant `18446744073709551615` has a type that is not implicitly convertible to the scrutinee type
// CHECK-SAME: (§5.1)
