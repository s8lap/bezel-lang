// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, expr_body_property_getter_invalidates_capture_spec_7_4).
//
// §7.4 (#1593): a user-defined expression-body property getter (`=> e`) between
// the test and the use is an accessor call — it can transfer control to user
// code, so it invalidates a mutably-captured local's narrowing. Before the fix
// this compiled clean (the getter read was not a recognised control transfer).

module M; class Box public property Val: int => 42; end Box; class C public static method Run(box: Box): int var s: string? := "hi"; var f := lambda() => do s := null; end; if s <> null then _ := box.Val; return s.Length; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
