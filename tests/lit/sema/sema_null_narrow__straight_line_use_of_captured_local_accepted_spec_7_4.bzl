// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, straight_line_use_of_captured_local_accepted_spec_7_4).
//
// §7.4 (#1566): straight-line check-then-use is preserved. Even for a mutably-
// captured local, if there is no control-transfer operation BETWEEN the test and
// the use, the narrowing holds — this must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Run(): int var s: string? := "hi"; var f := lambda() => do s := null; end; if s <> null then return s.Length; end; return -1; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
