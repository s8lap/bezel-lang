// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule4_static_readonly_autoprop_class_warns).
//
// (18) §22.12 rule 4: a static readonly auto-property of user class type warns.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Registry public X: int := 0; end Registry; class App public static readonly property Instance: Registry?; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} static readonly property `Instance` has class type `Registry` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization
// CHECK-SAME: (§22.12 rule 4, #1363)
