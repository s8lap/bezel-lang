// issue #2794 — stdlib §5: `IGrouping<TKey, T>` was not in
// `Resolver::builtinTypeArity`'s hand-list, so any source-level reference
// (e.g. a helper method's parameter type, stdlib §5's public grouping
// interface) failed to resolve with "unresolved type name `IGrouping`".
//
// Spec authority:
//   - stdlib §5 ("GroupBy"): `GroupBy<TKey>(key: (T) -> TKey): IEnumerable<
//     IGrouping<TKey, T>>` and `public interface IGrouping<TKey, T>:
//     IEnumerable<T>` — a helper method that takes or returns an IGrouping
//     is spec-legal (and is exactly the §24 sibling of fix #1536 for
//     EventSource<T>).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; public class C public static method TakeGroup(g: IGrouping<int, string>?): int return 42; end TakeGroup; public static method Main(): int return C.TakeGroup(null); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: unresolved type name
