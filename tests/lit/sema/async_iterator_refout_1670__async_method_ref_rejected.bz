// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, async_method_ref_rejected).
//
// A `ref` parameter on an `async` method — rejected. Fails without the fix
// (the compiler previously accepted this declaration outright). // per §30.1

module M; class Foo public async method Bump(ref x: int): Task x := x + 1; return; end Bump; end Foo;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `ref` parameter `x` is not permitted on an `async` method `Bump` — its arguments are captured into a heap-allocated state-machine frame that outlives the synchronous call, so a caller-stack-slot reference would dangle the moment the method returns its task or enumerable at the first suspension point
// CHECK-SAME: (§30.1, §29.3, v0.184)
