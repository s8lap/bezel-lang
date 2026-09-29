// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, static_owned_readonly_field_ok_1557).
//
// §11.7 (v0.181, issue #1557): lifting an unrecorded leftover of the v0.55
// ban, a static class MAY declare a `static owned readonly` field — the
// canonical resource-holding static (§11.5) — because static exit disposal
// (§22.13) is runtime-driven and independent of instance `Dispose()`, so the
// no-instantiation reasoning that forbids instance members never applied to
// it. Before this fix, this was rejected with "static class `Globals` may
// not declare an owned field `Held` (§11.7)".
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Held end Held; static class Globals public static owned readonly Cache: Held := new Held(); end Globals;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} static readonly field `Cache` has class type `Held` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization
// CHECK-SAME: (§22.12 rule 4, #1363)
