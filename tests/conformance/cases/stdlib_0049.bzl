public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T>, IOwningCollectionLiteralTarget<OwningArray<T>, T> where T: class
                                     // IOwningEnumerable: stated v0.195 — §4.13/spec §29.2's both-implemented rule presupposed it
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningArray<T>;   // §2.19 witness: the
                                                                                                        //   transferred array is the result
  public constructor();
  public constructor(capacity: int);

  public pure readonly property Count: int;
  public pure readonly property Capacity: int;

  public property [i: int]: T
    get;                                     // borrow of slot
    set(owned);                              // takes owned T; disposes previous value
  end;

  public method Add(owned item: T);
  public method AddRange(owned items: OwningArray<T>);   // drains items into self
  public method Insert(index: int, owned item: T);
  public method RemoveAt(index: int);                    // disposes the removed element
  public method Remove(item: T): bool;                   // borrow-taking query;
                                                          //   disposes the matched element
  public method Clear();                                  // disposes every element
  public method Contains(item: T): bool;
  public method IndexOf(item: T): int;

  public method Sort(comparer: (T, T) -> int);          // comparer takes borrows
  public method Sort();                                 // natural order; conditionally
                                                        //   admitted where `T: IComparable<T>`
                                                        //   (language spec §14.2) -- see #3325,
                                                        //   the gate is Array-only today
  public method Reverse();

  // Drain methods — transfer ownership of elements out of the collection
  public method PopAt(index: int): owned T;             // removes and returns owned
  public method PopFirst(): owned T;                     // = PopAt(0); raises if empty
  public method PopLast(): owned T;                      // = PopAt(Count - 1); raises if empty
  public method TryPopFirst(out owned item: T?): bool;   // v0.171 (#1336): the false path assigns null (§22.3)
  public method TryPopLast(out owned item: T?): bool;
  public method Drain(): IOwningEnumerable<T>;             // iteration drains the source
end OwningArray;

// No `OwningArray<T>` extension supplies a default-comparer `Sort`. Spec §26.5
// rule 1 commits instance resolution by *name*, and `OwningArray<T>` declares
// `Sort` above, so an extension member of that name is never consulted. Callers
// pass a comparer, or call the parameterless `Sort()` declared above, admitted
// where `T: IComparable<T>` (restored v0.276).
