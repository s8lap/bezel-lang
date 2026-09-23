// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, static_field_in_static_class_ok).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal), but it DOES emit a diagnostic — `_count` is a mutable static of
// primitive type, which §22.12 rule 2 form (iii) (#1393 warned-admission
// ruling, v0.221) admits only with a declaration-site warning; unrelated
// to this test's own subject (a static field is legal inside a `static
// class`), so only pinned here to keep the ACCEPT assertion accurate
// rather than silently stale.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; static class Utils public static _count: int := 0; end Utils;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `_count` has type `int` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
