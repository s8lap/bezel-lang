// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, unqualified_instance_method_ref_entry_rejected_1492).
//
// fix #1492 / §30.14: an unqualified reference to an INSTANCE method
// (implicit `this` receiver) is still rejected — only a static method
// qualifies as capture-free, whether written qualified or unqualified;
// dropping the qualifier must not smuggle an instance method past the
// classifier.

module M; class Worker public method Run() end Run; public async method Go(): Task using var t := Domain.Spawn(Run); await t; end Go; end Worker;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Domain.Spawn entry must be capture-free: a non-capturing lambda or a static method reference — a capturing lambda or an instance method reference is rejected because captures and receivers are borrows of the origin domain
// CHECK-SAME: (§30.14, #600)
