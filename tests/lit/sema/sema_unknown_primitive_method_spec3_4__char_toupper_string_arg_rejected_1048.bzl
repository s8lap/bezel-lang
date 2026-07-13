// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4, char_toupper_string_arg_rejected_1048).
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (see `char_repeat_is_rejected.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `checkBody` name specifically).
//
// #1048: a `string` argument to char.ToUpper (expects `char`) was
// previously accepted and crashed codegen. Must now be rejected.

module M; class C public static method Main(): int var c := char.ToUpper("x"); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: 'char.ToUpper' requires a `char` argument
// CHECK-SAME: (stdlib §9.5/§9.6)
