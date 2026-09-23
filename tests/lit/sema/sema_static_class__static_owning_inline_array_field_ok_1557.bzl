// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, static_owning_inline_array_field_ok_1557).
//
// §11.7 (#1557): a static owning-inline-array field (`[N owned]T`) on a
// static class is accepted. This combination never carried the field-level
// `owned` modifier the old rejection inspected (the array's `owned` marker
// lives in the TYPE, `[N owned]T` — §15.3), so it was never rejected even
// before this fix; this locks the acceptance in as a regression guard.
//
// Legitimate test update for §22.12 rule 2 form (iv) (#1393 warned-admission
// ruling, v0.221): a mutable owning-inline-array static FIELD is admitted
// with a declaration-site WARNING (rule 2 was, until #1393 landed, entirely
// silent on this shape — the old NODIAG-strict assertion below predates the
// ruling and is now stale, not merely coincidentally different). §1557's
// ACCEPTANCE is unchanged — bezelc -c still exits zero — only the "and emits
// NO diagnostic" half of the old assertion is corrected.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero, but it DOES emit a
// diagnostic — pinned below.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Slot end Slot; static class Globals public static _arr: [2 owned]Slot? := [new Slot(), new Slot()]; end Globals;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `_arr` has owning-inline-array type `[2 owned]Slot?` — admitted with a declaration-site warning (§22.12 rule 2 form (iv))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
