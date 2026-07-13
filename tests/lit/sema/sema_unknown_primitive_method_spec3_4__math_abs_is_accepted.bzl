// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4, math_abs_is_accepted).
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (see `char_repeat_is_rejected.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `checkBody` name specifically).
//
// stdlib §9: Math.Abs(x: double): double — a Math builtin, not a primitive
// static, but must remain accepted (control against over-rejection).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var a := Math.Abs(0 - 5); return a; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
