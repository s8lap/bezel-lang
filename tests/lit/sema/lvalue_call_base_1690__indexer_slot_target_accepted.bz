// Ported from tests/sema/lvalue_call_base_1690_test.cpp
//   (BEZEL_TEST lvalue_call_base_1690, indexer_slot_target_accepted).
//
// Positive control: an indexer slot on a plain-identifier base is unaffected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public owned items: Array<int>; public constructor() items := new Array<int>(3); end constructor; public static method Main(): int using var h := new C(); h.items[0] += 1; return h.items[0]; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
