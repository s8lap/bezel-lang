// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_indexer_747_test.cpp
//   (BEZEL_TEST static_indexer_747, static_indexer_rejected).
//
// §23.5: a static indexer is rejected.

module M; class C public static property [i: int]: int => i * 2; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static indexers are not supported
// CHECK-SAME: (§23.5)
