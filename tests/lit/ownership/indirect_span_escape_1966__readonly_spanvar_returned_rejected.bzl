// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (issue #1966): the read-only span twin of the indirect
// escape — `var s := local.AsReadOnlySpan(); return s;`. ReadOnlyArraySpan is a
// borrow of its backing Array exactly as ArraySpan is, so the one-hop-through-a-
// local escape dangles identically (UAF before the fix). Confirms the fix is not
// specific to the writable `AsSpan()` producer.

module M; class C public static method Leak(): ReadOnlyArraySpan<int> var local := new Array<int>(); local.Add(7); var s := local.AsReadOnlySpan(); return s; end Leak; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned `s` is a span viewing `local` whose backing storage is disposed at scope end
// CHECK-SAME: (§9.7 borrow-lifetime rule
