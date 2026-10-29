// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4, string_repeat_string_arg_rejected_1048),
//   RETARGETED by fix #7129 (stdlib §9.6, v0.350): `string.Repeat("x", 3)` is the DECLARED
//   `Repeat(unit: string, count: int)` overload and is now ADMITTED, so the row pins the
//   refusal that survives -- a first argument that is neither a `char` nor a `string`.
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (see `char_repeat_is_rejected.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `checkBody` name specifically).
//
// #1048 / §9.5 / §9.6: the char/int-expecting primitive static methods
// (string.Repeat(c: char, count: int); char classification/transform
// statics) validate their argument types. A `string` argument where a
// `char` is expected was previously accepted and crashed codegen. A `double`
// first argument (neither declared overload's first parameter) must stay
// rejected.

module M; class C public static method Main(): int var s := string.Repeat(1.5, 3); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: 'string.Repeat' requires a `char` or `string` first argument
// CHECK-SAME: (stdlib §9.5/§9.6)
