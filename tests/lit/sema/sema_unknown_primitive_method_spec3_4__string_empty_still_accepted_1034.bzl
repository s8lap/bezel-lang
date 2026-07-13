// Ported from tests/sema/unknown_primitive_method_test.cpp
//   (BEZEL_TEST sema_unknown_primitive_method_spec3_4,
//   string_empty_and_from_still_accepted_1034 — first of two independent
//   checkBody() assertions in that BEZEL_TEST body, split into its own .bzl
//   per the extractor's manual-split convention).
//
// Source built by hand from the file's local `checkBody(body)` C++ helper
// (see `char_repeat_is_rejected.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `checkBody` name specifically).
//
// #1034 regression: the REAL string statics (§9.6 Join/From/Empty/Repeat)
// keep working — they must NOT be over-rejected by the Format/Concat
// removal. This half checks `string.Empty`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var s := string.Empty; return s.Length; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
