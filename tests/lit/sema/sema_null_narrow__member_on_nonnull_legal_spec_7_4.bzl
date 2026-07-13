// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, member_on_nonnull_legal_spec_7_4).
//
// §7.4: a NON-nullable value's member access is of course still legal (the
// enforcement must not over-reject the common case).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class User public Name: int := 0; end User; class C public static method Run(user: User): int return user.Name; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
