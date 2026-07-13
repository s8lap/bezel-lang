// Ported from tests/sema/abstract_instantiation_test.cpp
//   (BEZEL_TEST sema_inst, new_non_abstract_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A end A; class C public method M() _ := new A(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
