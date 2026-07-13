// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/string_concat_746_test.cpp
//   (BEZEL_TEST string_concat_746, string_plus_double_rejected).
//
// Source built by hand from the file's `mainOf(expr)` C++ helper (a
// `const char*`-parameter string builder the extractor's automatic
// string-helper resolver does not match) — expanded here to
// `mainOf("\"v=\" + 1.5")`.
//
// §4.6: `string + double` (was an IR_FAIL) is rejected.

module M; static class F public static method Main(): int   var s := "v=" + 1.5; return s.Length; end Main; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `+` requires both operands to be `string` — the compiler does not implicitly convert other types to `string`; use interpolation (`$"...{x}..."`) or `.ToString()`
// CHECK-SAME: (§4.6)
