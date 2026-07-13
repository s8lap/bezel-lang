// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, bang_unwrap_member_legal_spec_7_8).
//
// §7.8 / §7.4: `x!.field` — the force-unwrap yields the non-nullable form, so
// member access is legal without an `if` guard.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class User public Name: int := 0; end User; class C public static method Run(user: User?): int return user!.Name; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
