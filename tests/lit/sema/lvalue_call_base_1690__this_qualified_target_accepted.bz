// Ported from tests/sema/lvalue_call_base_1690_test.cpp
//   (BEZEL_TEST lvalue_call_base_1690, this_qualified_target_accepted).
//
// Positive control: a `this`-qualified target inside the declaring class is
// unaffected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public total: int := 0; public method Bump() this.total += 5; end Bump; public static method Main(): int using var h := new C(); h.Bump(); return h.total; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
