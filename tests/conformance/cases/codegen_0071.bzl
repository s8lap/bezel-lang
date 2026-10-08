dict.Set(key, value);              // ERROR: no `Set` method on ANY collection
dict[missing] += 1;                // throws KeyNotFoundException: the indexer updates, never inserts
var l := new List<int>();          // no `List<T>` — the name is reserved
arr.Count(pred);                   // does NOT reach the predicate count on an Array<T> receiver
var s := "n = " + count;           // ERROR: no implicit ToString in `+`
pool.Sorted();                     // OwningArray<T> declares no Sorted/Reversed copy-forms
var text := String.Format("{0}", x);   // no String.Format
if a < b then ...                    // ERROR when either is `int?` (section 7.8)
