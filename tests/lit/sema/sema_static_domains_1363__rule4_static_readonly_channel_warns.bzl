// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule4_static_readonly_channel_warns).
//
// (25) §22.12 rule 4 (v0.181, #1556): a `static readonly Channel<T>` still draws
// the declaration-site warning (Channel is excluded from the form-(i) set, so
// the warning is not suppressed) — the reminder that the channel is the
// single-consumer receiving end and must stay confined to one domain (§30.15).
// It is a warning, not an error.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class App public static readonly _ch: Channel<int> := new Channel<int>(1); public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} static readonly field `_ch` has class type `Channel` which is reachable from every domain; confine its use to one domain or ensure its own internal synchronization
// CHECK-SAME: (§22.12 rule 4, #1363)
