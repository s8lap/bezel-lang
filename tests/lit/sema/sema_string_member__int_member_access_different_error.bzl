// Ported from tests/sema/string_member_diagnostic_test.cpp
//   (BEZEL_TEST sema_string_member, int_member_access_different_error).
//
// Source built by hand from the file's local `check(body)` C++ helper (see
// `length_is_accepted.bzl` for why the extractor's automatic resolution is
// unsafe for this file's `check` name specifically).
//
// Member access on a non-string primitive (e.g. int) is not mis-diagnosed.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var n := 5; return n; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
