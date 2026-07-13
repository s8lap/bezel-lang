// Ported from tests/sema/shared_weak_instantiation_603_test.cpp
//   (BEZEL_TEST sema_shared_weak_instantiation_603, spec_28_13_shared_of_class_accepted).
//
// §28.13 (must-pass): Shared<T> where T is a user-defined class is valid.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Inner end Inner; class C public static method M() var obj := new Inner(); using var h := new Shared<Inner>(owned obj); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
