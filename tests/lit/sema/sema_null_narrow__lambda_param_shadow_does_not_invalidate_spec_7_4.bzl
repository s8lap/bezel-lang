// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, lambda_param_shadow_does_not_invalidate_spec_7_4).
//
// §7.4 (#1566): NO over-invalidation from shadowing. A lambda whose OWN parameter
// shadows the outer name assigns only its parameter, not the outer local — so the
// outer local is not mutably captured and its narrowing survives a call.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Run(): int var s: string? := "hi"; var f := lambda(s: string?) => do s := null; end; if s <> null then f(s); return s.Length; end; return -1; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
