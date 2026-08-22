public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T> where T: class
  constructor();  constructor(capacity: int);
  pure readonly property Count: int;   Capacity: int;
  property [i: int]: T { get;  set(owned); }          // get = borrow of the slot; set disposes previous
  method Add(owned item: T);   AddRange(owned items: OwningArray<T>);   // AddRange DRAINS items
  method Insert(index: int, owned item: T);
  method RemoveAt(index: int);   Remove(item: T): bool;   Clear();      // all dispose
  method Contains(item: T): bool;   IndexOf(item: T): int;
  method Sort(comparer: (T, T) -> int);   Sort();   Reverse();   // comparer takes borrows;
                                                                //   bare Sort() is natural order,
                                                                //   only where T: IComparable<T>
  method PopAt(index: int): owned T;   PopFirst(): owned T;   PopLast(): owned T;   // raise if empty
  method TryPopFirst(out owned item: T?): bool;   TryPopLast(out owned item: T?): bool;
  method Drain(): IOwningEnumerable<T>;
end
// No `OwningArray<T>` extension supplies a default-comparer `Sort`: §26.5 rule 1
// commits instance resolution by *name*, and the class declares `Sort` above, so an
// extension member of that name is never consulted. Callers pass a comparer, or
// call the parameterless `Sort()` above where `T: IComparable<T>` (v0.276).
