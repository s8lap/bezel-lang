// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/untyped_collection_literal_test.cpp
//   (BEZEL_TEST collection_literal_interface_arg_1174, collection_literal_string_join_arg_rejected_1174).
//
// §15.4 (#1174): a collection literal as the `IEnumerable<string>` argument to
// the builtin `string.Join(sep, items)` (stdlib §3.4) is rejected.

module M; class C public static method M(): int var s := string.Join("-", ["a", "b", "c"]); return s.Length; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a collection literal `[...]` cannot target the interface type `IEnumerable<string>` — §15.4 admits only a concrete collection target (an inline array, `Array<T>`/`OwningArray<T>`, `Set`, `Stack`, `Queue`, `LinkedList`, `Dictionary`); construct one of those and pass it
// CHECK-SAME: (§15.4)
