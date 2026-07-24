// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §35.4 (v0.226, issue #2183): the RESULT-type axis. A borrow-bearing
// choice instantiation may not be an `async` method's result/element type — it
// would escape the state-machine frame with no bounded source. `Task<Opt<
// ArraySpan<int>>>` returns a span-carrying choice.

module M; choice Opt<T> Some(v: T), None; end Opt; static class F public static async method Bad(): Task<Opt<ArraySpan<int>>> return Opt<ArraySpan<int>>.None; end Bad; end F; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: an `async` method may not have a borrow-bearing choice result or element type
// CHECK-SAME: (§30.1
