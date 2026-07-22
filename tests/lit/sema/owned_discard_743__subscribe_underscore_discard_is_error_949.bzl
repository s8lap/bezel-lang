// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, subscribe_underscore_discard_is_error_949).
//
// REJECT: `_ :=` discard of a Subscribe result (§17.13).

module M; public class Button public event Clicked: int; public method Click(v: int) Clicked.Raise(v); end Click; end Button; public class C public static method Main(): int using var btn: Button := new Button(); _ := btn.Clicked.Subscribe(lambda(v) => do end); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot discard an event subscription value — the subscription would immediately unsubscribe; bind it (`using var _ := expr;`) or store it (`var sub := expr;`) to keep it alive
// CHECK-SAME: (§17.13 / §2.11)
