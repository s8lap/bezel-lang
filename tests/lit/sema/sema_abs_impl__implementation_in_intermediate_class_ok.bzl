// Ported from tests/sema/abstract_impl_test.cpp
//   (BEZEL_TEST sema_abs_impl, implementation_in_intermediate_class_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; abstract class A public abstract method Draw(); end A; class B : A public override method Draw() _ := 1; end Draw; end B; class C : B end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
