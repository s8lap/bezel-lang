// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, subscribe_statement_drop_is_error_949).
//
// REJECT: bare-statement drop of a Subscribe result (§21.9).

module M; public class Button public event Clicked: int; public method Click(v: int) Clicked.Raise(v); end Click; end Button; public class C public static method Main(): int using var btn: Button := new Button(); btn.Clicked.Subscribe(lambda(v) => do end); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this expression produces an owned value that requires disposal (a non-trivial-dispose class or a move-only owning array); discarding it as a statement leaks the resource — bind it (`using var _ := expression;`), call `.Dispose()`, or transfer it (`move`/`return`)
// CHECK-SAME: (§21.9)
