// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, uncaptured_local_keeps_narrowing_across_call_spec_7_4).
//
// §7.4 (#1566): NO over-invalidation. A local that is NOT captured by any lambda
// keeps its narrowing across a call (the by-value #1188 guarantee), even though
// the body contains a lambda that mutates a DIFFERENT local.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Touch() end Touch; public static method Run(): int var s: string? := "hi"; var other: string? := "x"; var f := lambda() => do other := null; end; if s <> null then Touch(); return s.Length; end; return -1; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
