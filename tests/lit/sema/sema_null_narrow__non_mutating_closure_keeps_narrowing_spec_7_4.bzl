// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, non_mutating_closure_keeps_narrowing_spec_7_4).
//
// §7.4 (#1566): NO over-invalidation. A closure that CAPTURES the local but only
// READS it (never assigns / ref/out-passes it) does not make the local mutably
// captured, so its narrowing survives a call — this must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Run(): int var s: string? := "hi"; var f := lambda() => do var z := s; end; if s <> null then f(); return s.Length; end; return -1; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
