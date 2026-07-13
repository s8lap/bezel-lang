// Ported from tests/sema/icomparable_iequatable_576_test.cpp
//   (BEZEL_TEST sema_576_icomparable_iequatable, icomparable_as_param_type_resolves_2_8).
//
// §2.8: IComparable<T> as a method parameter type resolves.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method Foo(x: IComparable<int>?): int return 0; end Foo; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
