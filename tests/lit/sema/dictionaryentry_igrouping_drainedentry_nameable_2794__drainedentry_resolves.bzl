// issue #2794 — stdlib §4.9: `DrainedEntry<K, V>` was not in
// `Resolver::builtinTypeArity`'s hand-list, so any source-level reference
// (e.g. a helper method's parameter type for an owning-dictionary drain
// entry) failed to resolve with "unresolved type name `DrainedEntry`".
//
// Spec authority:
//   - stdlib §4.9 ("OwningDictionary<K, V>"): declares `public method
//     Drain(): IOwningEnumerable<DrainedEntry<K, V>>;` and `public class
//     DrainedEntry<K, V>: IDisposable where V: class` — a helper that
//     takes a DrainedEntry as a parameter (e.g. to wrap the key in some
//     logging) is spec-legal.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; import System.Collections.Generic; class MyValue public readonly property N: int; public constructor(n: int) N := n; end constructor; end MyValue; public class C public static method TakeDrained(e: DrainedEntry<int, MyValue>): int return e.Key; end TakeDrained; public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: unresolved type name
