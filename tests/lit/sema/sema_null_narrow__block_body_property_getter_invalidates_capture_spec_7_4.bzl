// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, block_body_property_getter_invalidates_capture_spec_7_4).
//
// §7.4 (#1593): a block-body property with an explicit `get` (and `set`) arm is
// likewise a user accessor — reading it invalidates a mutably-captured local.

module M; class Box private _v: int := 0; public property Val: int get => _v; set => do _v := value; end; end; end Box; class C public static method Run(box: Box): int var s: string? := "hi"; var f := () => do s := null; end; if s <> null then _ := box.Val; return s.Length; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
