// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, named_fills_optional_others_default_ok).
//
// §22.5 example `Log("System started", timestamp := false)` — a named
// argument fills one optional parameter while another optional parameter
// (and any positional) takes its default. All required supplied → ok.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(a: int, b: int := 5, c: int := 9) end F; public method G() this.F(1, c := 7); end G; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
