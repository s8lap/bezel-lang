// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule4_static_readonly_class_warns).
//
// (13) §22.12 rule 4: a static readonly field of a user class type emits a
// warning.  The class is reachable from every domain; the programmer must
// confine its use to one domain or ensure internal synchronization.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cfg public X: int := 0; end Cfg; class App public static readonly _cfg: Cfg := new Cfg(); public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} static readonly field `_cfg` has class type `Cfg` which is reachable from every domain; confine its use to one domain or ensure its own internal synchronization
// CHECK-SAME: (§22.12 rule 4, #1363)
