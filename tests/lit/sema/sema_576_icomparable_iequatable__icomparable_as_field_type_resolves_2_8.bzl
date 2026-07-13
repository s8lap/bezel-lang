// Ported from tests/sema/icomparable_iequatable_576_test.cpp
//   (BEZEL_TEST sema_576_icomparable_iequatable, icomparable_as_field_type_resolves_2_8).
//
// §2.8: IComparable<T> as a field type resolves (arity 1).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _comp: IComparable<int>?; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
