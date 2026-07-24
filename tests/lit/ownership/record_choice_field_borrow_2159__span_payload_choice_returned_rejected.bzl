// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §35.4 / §4.15 (v0.226, issue #2159): a record whose choice field
// carries a builtin SPAN payload is borrow-bearing by transitivity — `SpanC`
// is a borrow-bearing choice (an unmarked `ArraySpan<int>` payload views its
// backing `Array`, §4.15), so `HolderC` holding it is a borrow-bearing record
// (§9.7). This is the memory-safety heart of the bug: before the fix the
// accepted return SEGFAULTED under ASan (dangling span read once `local`
// auto-drops) — the choice-field dual of #2127's span-field record escape.

module M; choice SpanC Some(s: ArraySpan<int>), None; end SpanC; record HolderC(K: SpanC); class C public static method Leak(): HolderC var local := new Array<int>(); local.Add(7); return new HolderC(SpanC.Some(local.AsSpan())); end Leak; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned borrow-bearing record carries a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§9.7 record borrow-lifetime rule
