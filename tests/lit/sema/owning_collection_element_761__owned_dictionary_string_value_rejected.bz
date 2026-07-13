// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/owning_collection_element_761_test.cpp
//   (BEZEL_TEST owning_collection_element_761, owned_dictionary_string_value_rejected).
//
// §14.4 / §28.11: `OwningDictionary<K, V>` owns its VALUES — a `string` VALUE is
// rejected (the key slot is value-like and unrestricted).

module M; class Foo public n: int := 0; end Foo; interface IFoo method F(): int; end IFoo; class C public static method Main(): int using var d := new OwningDictionary<int, string>(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `OwningDictionary` requires a class or interface element type (stdlib §4.2 / §14.4, #1327) — `string` (a runtime-managed reference type) has no disposal obligation and cannot be owned in an owning collection. Use `Dictionary<…>` for value elements.
