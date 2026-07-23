// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 (v0.220, issue #2145): the classification fix is GENERAL, not async-
// only — a `Wrap<ArraySpan<int>>` local returned from the method is the same
// §9.7 record borrow-escape as a concrete span-field record (#2127's indirect
// shape): the returned record's carried borrow of `local` outlives the §28.3
// auto-drop. Before the fix, `Wrap<…>` was never classified borrow-bearing,
// so the provenance was never collected and the return compiled clean.

module M; record Wrap<T>(V: T); class C public static method Leak(): Wrap<ArraySpan<int>> var local := new Array<int>(); local.Add(7); var w := new Wrap<ArraySpan<int>>(local.AsSpan()); return w; end Leak; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned `w` is a borrow-bearing record carrying a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§9.7 record borrow-lifetime rule
