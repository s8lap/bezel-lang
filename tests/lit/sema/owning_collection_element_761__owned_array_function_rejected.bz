// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/owning_collection_element_761_test.cpp
//   (BEZEL_TEST owning_collection_element_761, owned_array_function_rejected).
//
// §14.4: a function value is the other runtime-managed reference type rejected.

module M; class Foo public n: int := 0; end Foo; interface IFoo method F(): int; end IFoo; class C public static method Main(): int using var a := new OwningArray<() -> int>(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `OwningArray` requires a class or interface element type (stdlib §4.2 / §14.4, #1327) — a function value has no disposal obligation and cannot be owned in an owning collection. Use `Array<…>` for value elements.
