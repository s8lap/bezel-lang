// Ported from tests/sema/icomparable_iequatable_576_test.cpp
//   (BEZEL_TEST sema_576_icomparable_iequatable, where_iequatable_constraint_resolves_14_2).
//
// §2.9 / §14.2: `where T: IEquatable<T>` generic constraint resolves.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Container<T> where T: IEquatable<T> private _val: T?; public method Contains(x: T): bool if _val = null then return false; end; return _val!.Equals(x); end Contains; end Container;

// NODIAG-NOT: error
// NODIAG-NOT: §
