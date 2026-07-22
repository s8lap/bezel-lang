// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, async_lambda_out_rejected).
//
// An async lambda declaring an explicit `out` parameter — rejected. // per §30.6

module M; class Foo public static async method Run(): Task var f: (out int) -> Task := async lambda(out x: int) => do x := 5; return; end; return; end Run; end Foo;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `out` parameter `x` is not permitted on an `async` lambda — its parameters are captured into a heap-allocated state-machine frame that outlives the synchronous call, so a caller-stack-slot reference would dangle the moment the lambda returns its task at the first suspension point
// CHECK-SAME: (§30.6, v0.184)
