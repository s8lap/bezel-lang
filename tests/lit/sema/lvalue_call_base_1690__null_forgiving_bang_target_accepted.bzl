// Ported from tests/sema/lvalue_call_base_1690_test.cpp
//   (BEZEL_TEST lvalue_call_base_1690, null_forgiving_bang_target_accepted).
//
// Positive control (#206 regression guard): the null-forgiving `!` hop reads
// through the SAME storage location as its operand, not a fresh value — it
// must remain a valid lvalue base and must NOT be flagged by this check.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public total: int := 0; end C; class D public static method Main(): int using var x := new C(); var a: C? := x; a!.total += 5; return a!.total; end Main; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
