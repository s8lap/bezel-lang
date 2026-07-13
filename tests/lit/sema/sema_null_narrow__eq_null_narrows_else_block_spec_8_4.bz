// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, eq_null_narrows_else_block_spec_8_4).
//
// §8.4 / §7.4: `if user = null then ... else user.Name` — the else-block of
// `= null` narrows `user` to non-null.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class User public Name: int := 0; end User; class C public static method Run(user: User?): int if user = null then return -1; else return user.Name; end; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
