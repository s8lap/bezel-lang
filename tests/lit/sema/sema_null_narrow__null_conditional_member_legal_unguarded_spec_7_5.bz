// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, null_conditional_member_legal_unguarded_spec_7_5).
//
// §7.5: `x?.field` is the safe form and needs no guard. The result type is
// always nullable, so this must NOT raise the §7.4 member-access error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class User public Name: int := 0; end User; class C public static method Run(user: User?): int? return user?.Name; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
