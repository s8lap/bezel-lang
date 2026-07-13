// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4, int_frobnicate_is_rejected).
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (see `char_repeat_is_rejected.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `checkBody` name specifically).
//
// §3.4 / stdlib §5.2: int has no static method `Frobnicate`.

module M; class C public static method Main(): int var x := int.Frobnicate(42); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type 'int' has no static method or property 'Frobnicate'
// CHECK-SAME: (§3.4)
