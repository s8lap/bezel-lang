// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4, char_issymbol_is_rejected_1036).
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (see `char_repeat_is_rejected.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `checkBody` name specifically).
//
// #1036 / §3.4 / stdlib §9.5: `char.IsSymbol` is NOT a stdlib char
// classification method (the set is IsDigit/IsLetter/IsLetterOrDigit/
// IsWhiteSpace/IsUpper/IsLower/IsControl/IsPunctuation). It was wrongly
// accepted (typed bool) but had no codegen lowering, so it silently returned
// a constant false for every input. Must now reject like any unknown static.

module M; class C public static method Main(): int var b := char.IsSymbol('+'); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type 'char' has no static method or property 'IsSymbol'
// CHECK-SAME: (§3.4)
