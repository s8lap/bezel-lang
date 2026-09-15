// issue #4178 — arity check: the new builtin-type entry (`IOrderedEnumerable<T>`,
// arity 1, mirroring `IEnumerable<T>`/`IGrouping<TKey,T>`'s own registrations)
// must trip the same "generic-argument count does not match built-in type"
// diagnostic every other builtin arity mismatch already emits (mirrors
// issue #2794's `missing_arity_diagnosed` sibling test for DictionaryEntry).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module Probe; public class C public static method Bad(seq: IOrderedEnumerable<int, string>): int return 0; end Bad; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: generic-argument count does not match built-in type `IOrderedEnumerable`
