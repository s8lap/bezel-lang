// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, defaulted_arity_both_fill_defaults_still_ambiguous).
//
// §22.6 defaulted-arity tie-break — "two candidates that both fill defaults
// remain subject to the ambiguity rule (the compiler declines to guess
// which omission the caller meant)". `Log(a: int, b: int := 0)` and
// `Log(a: int, c: string := "x")` both bind only `a` for `Log(5)`, so both
// fill a default — the tie-break (which requires one side to fill NO
// defaults) does not apply, and the call stays ambiguous.

module M; class Foo public method Log(a: int, b: int := 0): int => a + b; public method Log(a: int, c: string := "x"): int => a; public method Use() _ := this.Log(5); end Use; end Foo;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: ambiguous call to `Log`: no overload is unambiguously better
// CHECK-SAME: (§22.6)
