// Ported from tests/sema/static_indexer_747_test.cpp
//   (BEZEL_TEST static_indexer_747, instance_indexer_ok).
//
// §23.5: an ordinary instance indexer is fine — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _d: Array<int> := [10, 20, 30]; public property [i: int]: int => _d[i]; public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
