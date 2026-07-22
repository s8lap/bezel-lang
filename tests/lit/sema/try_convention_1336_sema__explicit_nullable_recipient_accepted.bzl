// Ported from tests/sema/try_convention_1336_test.cpp
//   (BEZEL_TEST try_convention_1336_sema, explicit_nullable_recipient_accepted).
//
// (b) explicit NULLABLE recipient — ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int do const s: Stack<int> := [1]; var x: int? := null; var ok := s.TryPop(out x); end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
