// issue #2794 — stdlib §4.8 / §4.9 / §5: `DictionaryEntry<K, V>` was not in
// `Resolver::builtinTypeArity`'s hand-list, so any source-level reference
// (e.g. a helper method's parameter type, stdlib §4.8's public record)
// failed to resolve with "unresolved type name `DictionaryEntry`" — even
// though the type was inferable from a `for var entry in dict` loop.
//
// Spec authority:
//   - stdlib §4.8 ("Dictionary<K, V>"): declares `public record
//     DictionaryEntry<K, V>(Key: K, Value: V);` in System.Collections.Generic
//     and `Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>` so every
//     entry-shaped member of the dictionary surfaces as a DictionaryEntry
//     (stdlib §1's elided-interface-required-members rule).
//   - language spec §2.3 / §11.8: builtin names that resolve without a
//     user TypeDecl (e.g. EventSource<T>, fix #1536; Channel<T>; the
//     §4 collection types) compose parameter / field / return annotations
//     for helpers — DictionaryEntry is the same shape.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Probe; import System.Collections.Generic; public class C public static method SumEntry(e: DictionaryEntry<int, int>): int return e.Value + e.Key; end SumEntry; public static method Main(): int var d := new Dictionary<int, int>(); d.Add(10, 20); var total: int := 0; for var entry in d do total := total + C.SumEntry(entry); end for; return total; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: unresolved type name
