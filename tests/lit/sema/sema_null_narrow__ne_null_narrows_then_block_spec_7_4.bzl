// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, ne_null_narrows_then_block_spec_7_4).
//
// §7.4: `if user <> null then ... user.Name` — the canonical example. Inside
// the then-block `user` is narrowed to its non-nullable form `User`, so the
// access is legal and `user.Name` resolves against `User`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class User public Name: int := 0; end User; class C public static method Run(user: User?): int if user <> null then return user.Name; end; return -1; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
