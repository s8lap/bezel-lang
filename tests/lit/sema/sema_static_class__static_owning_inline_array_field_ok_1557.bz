// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, static_owning_inline_array_field_ok_1557).
//
// §11.7 (#1557): a static owning-inline-array field (`[N owned]T`) on a
// static class is accepted. This combination never carried the field-level
// `owned` modifier the old rejection inspected (the array's `owned` marker
// lives in the TYPE, `[N owned]T` — §15.3), so it was never rejected even
// before this fix; this locks the acceptance in as a regression guard.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Slot end Slot; static class Globals public static _arr: [2 owned]Slot? := [new Slot(), new Slot()]; end Globals;

// NODIAG-NOT: error
// NODIAG-NOT: §
