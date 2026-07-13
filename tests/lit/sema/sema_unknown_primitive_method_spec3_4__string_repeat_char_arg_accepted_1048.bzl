// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4, string_repeat_char_arg_accepted_1048).
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (see `char_repeat_is_rejected.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `checkBody` name specifically).
//
// #1048 positive control: correct char/int arguments to string.Repeat must
// STILL be accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var s := string.Repeat('x', 3); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
