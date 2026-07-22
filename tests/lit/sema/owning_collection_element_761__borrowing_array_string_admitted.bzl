// Ported from tests/sema/owning_collection_element_761_test.cpp
//   (BEZEL_TEST owning_collection_element_761, borrowing_array_string_admitted).
//
// §14.4: the BORROWING family is unrestricted — `Array<string>` is admitted
// (it stores borrows, not owned elements).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo public n: int := 0; end Foo; interface IFoo method F(): int; end IFoo; class C public static method Main(): int const a: Array<string> := ["x"]; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
