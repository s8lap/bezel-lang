// Migrated for v0.213 (§28.11, axiom A8 retired, #2081): the pre-v0.213
// asymmetric per-value disposal loop —
//     for var entry in _index do
//       entry.Value.Dispose();     // transit-exception element-borrow dispose
//     end;
//     _index.Clear();
// — is retired with axiom A8. A holder that owns its dictionary values declares
// OwningDictionary<K, V> (values owned, keys value-like); its Clear()/Dispose()
// cascades over the values, so no manual entry.Value.Dispose() pass exists or is
// needed. (Remains a skip fragment — `_index` is not declared here.)
_index.Clear();   // OwningDictionary<K, V>.Clear() disposes every value
