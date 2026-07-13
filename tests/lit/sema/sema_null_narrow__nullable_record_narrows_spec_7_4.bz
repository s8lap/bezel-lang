// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, nullable_record_narrows_spec_7_4).
//
// §7.4: a nullable VALUE type also narrows. Here a nullable record value read
// under a `<> null` guard.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Box(V: int); class C public static method Run(b: Box?): int if b <> null then return b.V; end; return -1; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
