// Ported from tests/sema/lvalue_call_base_1690_test.cpp
//   (BEZEL_TEST lvalue_call_base_1690, plain_field_target_accepted).
//
// Positive control: a plain field target (no call anywhere in the chain) is
// unaffected by the new check.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public total: int := 0; public static method Main(): int using var h := new C(); h.total += 5; return h.total; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
