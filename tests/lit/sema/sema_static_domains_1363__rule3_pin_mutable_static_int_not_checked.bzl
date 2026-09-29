// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule3_pin_mutable_static_int_not_checked).
//
// (12) §22.12 rule 2 form (iii) (#1393 warned-admission ruling, v0.221):
// mutable static int now WARNS. Legitimate test update, per the milestone
// workflow (mirrors the earlier v0.181 Channel<T> update in
// sema_static_domains_1363__rule2_mutable_static_channel_rejected_v0181.bzl):
// this pin previously asserted NO diagnostic at all — "primitive types are
// not checked by the current Rule 2 implementation; see #1363-followup" —
// which was exactly issue #1393's under-enforcement bug (a mutable static of
// primitive type was silently admitted with no diagnostic whatsoever,
// neither the declaration-site warning rule 2 form (iii) requires nor an
// error). The #1393 ruling and its enforcement land the warning; the old
// NODIAG assertion is now definitively wrong, not merely stale. Overlaps
// (deliberately) with the fuller
// sema_static_domains_1393__rule2_form3_mutable_static_int_field_warns.bzl
// pin — kept here too so this ORIGINAL #1363 pin location stays accurate
// rather than silently drifting out of sync with the code it names.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal), but it DOES emit a diagnostic — pinned below.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static _count: int := 0; public static method Main(): int => _count; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `_count` has type `int` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
