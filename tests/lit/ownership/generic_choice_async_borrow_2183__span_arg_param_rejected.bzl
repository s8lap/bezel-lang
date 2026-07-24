// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.1 / §35.4 / §9.7 (v0.226, issue #2183): a generic choice instantiated at a
// borrow-carrying type argument is borrow-bearing at THAT instantiation (§35.4.2
// with "record" read as "record or choice") and is forbidden as an `async`
// parameter (§30.1). `Opt<ArraySpan<int>>` carries a span borrow. This is the
// issue's exact repro. Before the fix the async check keyed only on
// isBorrowBearingRecordOrSpan (never true for a choice) AND classifyChoiceType's
// borrow leg answered a bare-`T` payload conservatively without consulting the
// instantiation, so the parameter compiled and a span dangled in the frame.

module M; choice Opt<T> Some(v: T), None; end Opt; static class F public static async method Bad(x: Opt<ArraySpan<int>>): Task<int> return 1; end Bad; end F; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `async` parameter `x` has a borrow-bearing choice type
// CHECK-SAME: (§30.1
