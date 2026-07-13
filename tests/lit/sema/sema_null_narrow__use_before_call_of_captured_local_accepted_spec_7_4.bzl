// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, use_before_call_of_captured_local_accepted_spec_7_4).
//
// §7.4 (#1566): a use that PRECEDES the call is accepted — a call's arguments are
// evaluated before control transfers, so a read before the invocation is under
// the still-valid narrowing.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Sink(n: int) end Sink; public static method Run(): int var s: string? := "hi"; var f := () => do s := null; end; if s <> null then Sink(s.Length); f(); end; return 0; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
