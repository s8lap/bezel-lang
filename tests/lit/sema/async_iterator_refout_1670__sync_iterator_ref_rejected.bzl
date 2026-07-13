// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, sync_iterator_ref_rejected).
//
// A `ref` parameter on a SYNCHRONOUS iterator method (`yield return` makes
// it an iterator, §29.3) — rejected. // per §29.3

module M; class Foo public method Gen(ref n: int): IEnumerable<int> n := n + 1; yield return n; end Gen; end Foo;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `ref` parameter `n` is not permitted on an iterator method `Gen` — its arguments are captured into a heap-allocated state-machine frame that outlives the synchronous call, so a caller-stack-slot reference would dangle the moment the method returns its task or enumerable at the first suspension point
// CHECK-SAME: (§30.1, §29.3, v0.184)
