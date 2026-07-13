// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, object_members_on_class_clean).
//
// §8.1 / §8.5: the universal Object members are valid on a user CLASS even
// without a user-declared override (inherited from Object) — must not reject.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Widget public _id: int; end Widget; class C public method M(w: Widget): int var s := w.ToString(); var h := w.GetHashCode(); var e := w.Equals(w); return h; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
