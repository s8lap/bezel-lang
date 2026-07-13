// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4, string_concat_is_rejected_1034).
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (see `char_repeat_is_rejected.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `checkBody` name specifically).
//
// #1034 / §3.4 / §9.6: `string.Concat` is not a stdlib string static (use
// string interpolation `$"{...}"` instead). Must be rejected.

module M; class C public static method Main(): int var s := string.Concat("a", "b"); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type 'string' has no static method or property 'Concat'
// CHECK-SAME: (§3.4)
