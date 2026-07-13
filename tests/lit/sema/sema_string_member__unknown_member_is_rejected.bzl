// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/string_member_diagnostic_test.cpp
//   (BEZEL_TEST sema_string_member, unknown_member_is_rejected).
//
// Source built by hand from the file's local `check(body)` C++ helper (see
// the sibling `length_is_accepted.bzl` for why the extractor's automatic
// resolution is unsafe for this file's `check` name specifically).
//
// An unknown string member still produces a diagnostic.

module M; class C public static method Main(): int var s := "hello"; return s.NonExistent; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: string member `.NonExistent` is not yet supported in codegen
