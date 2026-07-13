// Ported from tests/sema/owning_collection_element_761_test.cpp
//   (BEZEL_TEST owning_collection_element_761, owned_array_interface_admitted).
//
// §14.4: "interface types … OwningArray<IConnection> is admitted."
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo public n: int := 0; end Foo; interface IFoo method F(): int; end IFoo; class C public static method Main(): int using var a := new OwningArray<IFoo>(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
