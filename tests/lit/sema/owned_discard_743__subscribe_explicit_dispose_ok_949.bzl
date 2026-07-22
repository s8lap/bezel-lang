// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, subscribe_explicit_dispose_ok_949).
//
// ACCEPT: explicit `.Dispose()` of a Subscribe result.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public class Button public event Clicked: int; public method Click(v: int) Clicked.Raise(v); end Click; end Button; public class C public static method Main(): int using var btn: Button := new Button(); var s := btn.Clicked.Subscribe(lambda(v) => do end); s.Dispose(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
