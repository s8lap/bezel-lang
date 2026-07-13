// Ported from tests/sema/generic_method_constraint_callsite_746_test.cpp
//   (BEZEL_TEST method_constraint_callsite_746, object_members_on_unconstrained_t_accepted_1078).
//
// §14.2 (#1078): ANTI-OVER-REJECT — Object members (ToString, GetHashCode)
// on an unconstrained T are valid and must NOT be rejected.  Tests that the
// new `else` branch only fires for non-Object members.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class U public static method CheckObj<T>(x: T): int var s := x.ToString(); var h := x.GetHashCode(); return h; end CheckObj; end U; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
