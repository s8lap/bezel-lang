// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, named_arg_participates_in_overload_resolution_ok).
//
// §22.5 + §22.6: a named-argument call participates in overload selection.
// `F(value := n)` where n is int binds the `value` parameter on both
// overloads; the int overload is the exact match (§22.6), the long overload
// requires a widening conversion — so the call is unambiguously resolved and
// no error is produced.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(value: int): int => value; public method F(value: long): int => 0; public method G(): int var n: int := 3; return this.F(value := n); end G; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
