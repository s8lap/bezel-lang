// Ported from tests/sema/owning_collection_element_761_test.cpp
//   (BEZEL_TEST owning_collection_element_761, owned_array_nullable_class_admitted).
//
// FIX #4796 (§14.2, v0.307): owning-collection `where T: class` admits
// NON-NULLABLE reference types only (§14.2 row: "Non-nullable reference types
// only — classes ..., interfaces, `string`, function types"). A nullable
// element type `Foo?` is rejected even though the underlying class `Foo`
// satisfies `where T: class`. The §14.4 verbatim example admitting
// `OwningTask<Lock?>` is a different family (Pattern B task RESULT, where the
// `?` is the task's result nullability, not T substitution) — see the new
// gtest at tests/sema/owning_array_nullable_reject_4796_test.cpp.
//
// REJECT control: bezelc -c must exit non-zero with the §14.2/#4796 diagnostic.
//
// RUN: not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Foo public n: int := 0; end Foo; interface IFoo method F(): int; end IFoo; class C public static method Main(): int using var a := new OwningArray<Foo?>(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `OwningArray` requires a non-nullable element type
// CHECK-SAME: (§14.2 `where T: class` admits non-nullable reference types only;
// CHECK-SAME: §14.4, v0.307, #4796)
