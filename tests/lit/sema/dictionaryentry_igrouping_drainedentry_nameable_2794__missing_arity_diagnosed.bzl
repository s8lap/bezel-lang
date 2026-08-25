// issue #2794 — arity check: the three new builtin-type entries
// (DictionaryEntry<K, V>, DrainedEntry<K, V>, IGrouping<TKey, T>) are all
// arity-2; used with a wrong number of generic arguments they must trip the
// same "generic-argument count does not match built-in type" diagnostic that
// every other arity-2 builtin (Dictionary, OwningDictionary, SortedDictionary)
// already emits.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module Probe; public class C public static method Bad(d: DictionaryEntry<int>) return 0; end Bad; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: generic-argument count does not match built-in type `DictionaryEntry`
