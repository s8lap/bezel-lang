// issue #4178 — stdlib §5: `IOrderedEnumerable<T>` was not in
// `Resolver::builtinTypeArity`'s hand-list, so any source-level reference
// (e.g. a helper method's parameter type) failed to resolve with
// "unresolved type name `IOrderedEnumerable`" even though the type is fully
// functional when only ever INFERRED (`var sorted := xs.OrderBy(...)`).
//
// Spec authority:
//   - stdlib §5 ("Ordering"): `OrderBy<TKey>(key: (T) -> TKey):
//     IOrderedEnumerable<T> where TKey: IComparable<TKey>;` and `public
//     interface IOrderedEnumerable<T>: IEnumerable<T>` — a helper method
//     that takes an IOrderedEnumerable as a parameter is spec-legal, the
//     exact sibling of #2794's IGrouping fix.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; public class C public static method TakeOrdered(seq: IOrderedEnumerable<int>?): int return 42; end TakeOrdered; public static method Main(): int return C.TakeOrdered(null); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: unresolved type name
