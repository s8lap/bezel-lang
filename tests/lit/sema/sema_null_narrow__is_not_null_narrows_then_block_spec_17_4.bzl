// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, is_not_null_narrows_then_block_spec_17_4).
//
// §17.4 / §7.4: `is not null` desugars to `<> null` (parser), so it narrows
// the then-block identically.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class User public Name: int := 0; end User; class C public static method Run(user: User?): int if user is not null then return user.Name; end; return -1; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
