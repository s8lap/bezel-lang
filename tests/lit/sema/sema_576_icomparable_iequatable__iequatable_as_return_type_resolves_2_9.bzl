// Ported from tests/sema/icomparable_iequatable_576_test.cpp
//   (BEZEL_TEST sema_576_icomparable_iequatable, iequatable_as_return_type_resolves_2_9).
//
// §2.9: IEquatable<T> as method return type resolves.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method GetEq(): IEquatable<int>? return null; end GetEq; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
