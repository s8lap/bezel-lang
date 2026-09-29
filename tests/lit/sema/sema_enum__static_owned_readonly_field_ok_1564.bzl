// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, static_owned_readonly_field_ok_1564).
//
// §10.4: "A static readonly field of class type may be marked `owned` …
// enrolling it in §22.13's program-exit disposal." Before v0.181 the enum
// grammar's StaticReadonlyDecl production had no `owned` slot; A.4 now
// carries it. The parser already accepted `owned` generically (parseMember's
// modifier loop is shared by class and enum bodies) and checkEnumRestrictions
// never special-cased `owned`, so this was already accepted on HEAD — this
// test locks the acceptance in as a spec-derived regression guard.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Held end Held; enum Kind A, B; public static owned readonly Cache: Held := new Held(); end Kind;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} static readonly field `Cache` has class type `Held` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization
// CHECK-SAME: (§22.12 rule 4, #1363)
