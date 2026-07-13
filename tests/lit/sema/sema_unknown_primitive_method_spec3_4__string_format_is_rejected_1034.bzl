// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4, string_format_is_rejected_1034).
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (see `char_repeat_is_rejected.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `checkBody` name specifically).
//
// #1034 / §3.4 / §9.6: `string.Format` and `string.Concat` are NOT stdlib
// string statics (use string interpolation `$"{...}"` for composite
// formatting). They were erroneously typed `string` by sema but have no
// codegen lowering, so they compiled and then crashed (Format → SIGSEGV) /
// returned garbage (Concat). They must be rejected like any other unknown
// static method.

module M; class C public static method Main(): int var s := string.Format("v={0}", 5); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type 'string' has no static method or property 'Format'
// CHECK-SAME: (§3.4)
