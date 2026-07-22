// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, async_lambda_ref_rejected).
//
// An async lambda declaring an explicit `ref` parameter in its own param
// list — rejected, even though the local `f` never escapes the enclosing
// method (the frame-capture hazard is unconditional, not escape-dependent).
// // per §30.6

module M; class Foo public static async method Run(): Task var f: (ref int) -> Task := async lambda(ref x: int) => do x := x + 1; return; end; return; end Run; end Foo;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `ref` parameter `x` is not permitted on an `async` lambda — its parameters are captured into a heap-allocated state-machine frame that outlives the synchronous call, so a caller-stack-slot reference would dangle the moment the lambda returns its task at the first suspension point
// CHECK-SAME: (§30.6, v0.184)
