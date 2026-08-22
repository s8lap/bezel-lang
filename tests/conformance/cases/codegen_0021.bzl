public class Array<T>: IList<T>                      // borrowing, resizable
  constructor();  constructor(capacity: int);  constructor(size: int, fill: T);
  pure readonly property Count: int;   pure readonly property Capacity: int;   property [i: int]: T;
  method Add(item: T);  AddRange(items: IEnumerable<T>);  Insert(index, item);  RemoveAt(index);
  method Remove(item: T): bool;  Clear();  Contains(item: T): bool;
  method IndexOf(item: T): int;  IndexOf(item: T, startIndex: int): int;  LastIndexOf(item: T): int;
  method Sort(comparer: (T, T) -> int);   method Reverse();                        // in place
  method Sorted(comparer: (T, T) -> int): owned Array<T>;   Reversed(): owned Array<T>;
  method ToArray(): owned Array<T>;
  method AsSpan([start[, length]] | r: Range<int>): ArraySpan<T>;
  method AsReadOnlySpan([start[, length]] | r: Range<int>): ReadOnlyArraySpan<T>;
  method Slice(r: Range<int>): owned Array<T>;       // `arr[r]` is the compiler-recognized form
  method Sort(); method Sorted(): owned Array<T>;    // natural order; only where T: IComparable<T>,
                                                     //   checked at the call's instantiation (v0.275)
end
