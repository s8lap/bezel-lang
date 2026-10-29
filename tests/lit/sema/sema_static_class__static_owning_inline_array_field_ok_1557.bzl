// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, static_owning_inline_array_field_ok_1557).
//
// §11.7 (#1557): a static owning-inline-array field (`[N owned]T`) on a
// static class is accepted. This combination never carried the field-level
// `owned` modifier the old rejection inspected (the array's `owned` marker
// lives in the TYPE, `[N owned]T` -- §15.3), so it was never rejected even
// before this fix; this locks the acceptance in as a regression guard.
//
// Updated for §22.12 (v0.350, issue #7931): the one admitted static of
// owning-inline-array type is the `static readonly` field (note (h)); the
// mutable spelling (formerly the #1393 form-(iv) warned admission) is rule 2's
// closing error, pinned by
// sema_static_domains_1393__rule2_mutable_static_owning_array_field_is_error_7931.bzl.
// The `static readonly` field takes rule 4's declaration-site warning (table
// row "owning inline array ... warn -- 4(h)"), so the compile still exits zero
// and DOES emit a diagnostic -- pinned below.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Slot end Slot; static class Globals public static readonly _arr: [2 owned]Slot? := [new Slot(), new Slot()]; end Globals;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} static readonly field `_arr` has owning-inline-array type `[2 owned]Slot?`
// CHECK-SAME: (§22.12 rule 4 residual, v0.265, #3273 / #3290
