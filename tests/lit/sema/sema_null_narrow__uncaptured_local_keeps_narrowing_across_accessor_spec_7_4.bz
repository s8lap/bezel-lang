// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, uncaptured_local_keeps_narrowing_across_accessor_spec_7_4).
//
// §7.4 (#1593): NO over-invalidation. A local that is NOT mutably captured keeps
// its narrowing across a user accessor call, exactly as across an ordinary call
// (#1188). The lambda here mutates a DIFFERENT local. Must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box public property Val: int => 42; end Box; class C public static method Run(box: Box): int var s: string? := "hi"; var other: string? := "x"; var f := () => do other := null; end; if s <> null then _ := box.Val; return s.Length; end; return -1; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
