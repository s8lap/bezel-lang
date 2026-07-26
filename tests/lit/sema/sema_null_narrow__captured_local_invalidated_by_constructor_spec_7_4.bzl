// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, captured_local_invalidated_by_constructor_spec_7_4).
//
// §7.4 (#1566): a constructor (`new`) is likewise a control transfer, so it
// invalidates a mutably-captured local's narrowing.

module M; class Thing public constructor() end; end Thing; class C public static method Run(): int var s: string? := "hi"; var f := lambda() => do s := null; end; if s <> null then _ := new Thing(); return s.Length; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot narrow `T?` accessed through a mutably-captured local
// CHECK-SAME: (§7.4)
