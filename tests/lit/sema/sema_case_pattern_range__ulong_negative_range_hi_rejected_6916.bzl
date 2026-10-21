// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #6916 (§19.3, §5.1, v0.340): a negative range upper bound over a ulong scrutinee.
// Reported repro shape: `var u: ulong := 3; case u of ..-9223372036854775808L`.

module M; class C public static method M(): int var u: ulong := 3; return (case u of ..-9223372036854775808L => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `case` pattern constant `-9223372036854775808` has a type that is not implicitly convertible to the scrutinee type
// CHECK-SAME: (§5.1)
