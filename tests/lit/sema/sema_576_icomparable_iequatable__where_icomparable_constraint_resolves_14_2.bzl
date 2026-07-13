// Ported from tests/sema/icomparable_iequatable_576_test.cpp
//   (BEZEL_TEST sema_576_icomparable_iequatable, where_icomparable_constraint_resolves_14_2).
//
// §2.8 / §14.2: `where T: IComparable<T>` generic constraint resolves.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Sorter<T> where T: IComparable<T> public method Compare(a: T, b: T): int return a.CompareTo(b); end Compare; end Sorter;

// NODIAG-NOT: error
// NODIAG-NOT: §
