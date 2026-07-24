// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §35.4.2 (v0.226, issue #2183): the class-substituted axis. `Opt<Res>`
// substitutes an unmarked class payload, which §35.4.2 makes borrow-bearing (a
// plain class reference is a borrow), so the choice is borrow-bearing at this
// instantiation and forbidden as an `async` parameter (§30.1).

module M; choice Opt<T> Some(v: T), None; end Opt; class Res public method V(): int return 7; end V; end Res; static class F public static async method Bad(x: Opt<Res>): Task<int> return 1; end Bad; end F; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `async` parameter `x` has a borrow-bearing choice type
// CHECK-SAME: (§30.1
