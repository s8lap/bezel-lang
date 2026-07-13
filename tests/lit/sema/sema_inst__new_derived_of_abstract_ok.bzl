// Ported from tests/sema/abstract_instantiation_test.cpp
//   (BEZEL_TEST sema_inst, new_derived_of_abstract_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; abstract class A end A; class B : A end B; class C public method M() _ := new B(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
