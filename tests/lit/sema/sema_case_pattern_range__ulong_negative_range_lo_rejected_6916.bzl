// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #6916 (§19.3, §5.1, v0.340): a negative range lower bound over a ulong scrutinee is the §5.1 error, as over uint.
// Reported repro shape: `var u: ulong := 3; case u of -5..10`.

module M; class C public static method M(): int var u: ulong := 3; return (case u of -5..10 => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `case` pattern constant `-5` has a type that is not implicitly convertible to the scrutinee type
// CHECK-SAME: (§5.1)
