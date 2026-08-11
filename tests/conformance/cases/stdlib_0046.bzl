public class Array<T>: IList<T>
  public constructor();
  public constructor(capacity: int);                     // empty (Count = 0), storage reserved;
                                                          //   capacity < 0: ArgumentOutOfRangeException (v0.183, #1396)
  public constructor(size: int, fill: T);                // Count = size, every slot = fill (v0.165, #1375);
                                                          //   size < 0: ArgumentOutOfRangeException (v0.183, #1396)

  public pure readonly property Count: int;
  public pure readonly property Capacity: int;
  public property [i: int]: T;

  public method Add(item: T);
  public method AddRange(items: IEnumerable<T>);
  public method Insert(index: int, item: T);
  public method RemoveAt(index: int);
  public method Remove(item: T): bool;
  public method Clear();
  public method Contains(item: T): bool;
  public method IndexOf(item: T): int;          // -1 if not found
  public method IndexOf(item: T, startIndex: int): int;
                                                // first occurrence at or after startIndex, or -1
                                                //   (v0.228); 0 <= startIndex <= Count required,
                                                //   IndexOutOfRangeException otherwise;
                                                //   startIndex = Count is legal and returns -1
  public method LastIndexOf(item: T): int;      // last occurrence, or -1 (v0.228). Equality is
                                                //   Equals for both, as the one-argument IndexOf

  public method Sort(comparer: (T, T) -> int);  // in place
  public method Reverse();                       // in place

  public method Sorted(comparer: (T, T) -> int): owned Array<T>;  // fresh sorted copy
  public method Reversed(): owned Array<T>;                       // fresh reversed copy

  public method ToArray(): owned Array<T>;      // fresh heap-array copy

  // Span views (v0.107)
  public method AsSpan(): ArraySpan<T>;                          // full array, writable
  public method AsSpan(start: int): ArraySpan<T>;                // from offset to end
  public method AsSpan(start: int, length: int): ArraySpan<T>;
  public method AsSpan(r: Range<int>): ArraySpan<T>;             // parallels Range slicing

  public method AsReadOnlySpan(): ReadOnlyArraySpan<T>;
  public method AsReadOnlySpan(start: int): ReadOnlyArraySpan<T>;
  public method AsReadOnlySpan(start: int, length: int): ReadOnlyArraySpan<T>;
  public method AsReadOnlySpan(r: Range<int>): ReadOnlyArraySpan<T>;

  // Range slicing (language spec §15.6): `arr[r]` — arr[a..b], arr[..b], arr[a..], arr[..] —
  // is the compiler-recognized equivalent of this method (v0.182):
  public method Slice(r: Range<int>): owned Array<T>;   // fresh array; the range's values, or copies of the element borrows
end Array;

public extension Array<T> where T: IComparable<T>
  public method Sort();                    // in place
  public method Sorted(): owned Array<T>;  // fresh sorted copy
end;
