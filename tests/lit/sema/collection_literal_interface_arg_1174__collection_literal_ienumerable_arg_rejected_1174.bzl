// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/untyped_collection_literal_test.cpp
//   (BEZEL_TEST collection_literal_interface_arg_1174, collection_literal_ienumerable_arg_rejected_1174).
//
// §15.4 (#1174): a collection literal as the argument to a user method whose
// parameter is `IEnumerable<char>` is rejected.

module M; class C public static method Count(xs: IEnumerable<char>): int var n := 0; for var c in xs do n := n + 1; end; return n; end Count; public static method Main(): int return Count(['x', 'y', 'z']); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a collection literal `[...]` cannot target the interface type `IEnumerable` — §15.4 admits only a concrete collection target (an inline array, `Array<T>`/`OwningArray<T>`, `Set`, `Stack`, `Queue`, `LinkedList`, `Dictionary`); construct one of those and pass it
// CHECK-SAME: (§15.4)
