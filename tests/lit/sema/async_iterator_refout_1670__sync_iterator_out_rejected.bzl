// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, sync_iterator_out_rejected).
//
// An `out` parameter on a synchronous iterator method — rejected. // per §29.3

module M; class Foo public method Gen(out n: int): IEnumerable<int> n := 3; yield return n; end Gen; end Foo;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `out` parameter `n` is not permitted on an iterator method `Gen` — its arguments are captured into a heap-allocated state-machine frame that outlives the synchronous call, so a caller-stack-slot reference would dangle the moment the method returns its task or enumerable at the first suspension point
// CHECK-SAME: (§30.1, §29.3, v0.184)
