public class Array<T>: IList<T>, ICollectionLiteralTarget<Array<T>, T>
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Array<T>;   // §2.19 witness; the literal's
                                                                                                //   own lowering (spec §15.4)
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

  // Natural order; admitted only where `T: IComparable<T>` (see the note below).
  public method Sort();                    // in place
  public method Sorted(): owned Array<T>;  // fresh sorted copy
end Array;

// Natural-order sorting is declared on `Array<T>` itself, NOT as an extension:
// spec §26.5 rule 1 commits instance resolution by *name* — of any arity — so an
// extension member spelled `Sort`/`Sorted` would never be consulted on an `Array<T>`
// receiver. These are arity-0 overloads of the names above (§22.6), and CONDITIONALLY
// ADMITTED members in the sense of language spec §14.2 (Conditionally-admitted
// members): admitted only where `T: IComparable<T>`, with the condition decided after
// substitution from the receiver's type argument and the rejection reported at the
// call, never at the declaration. §14.2's register carries the row -- and names
// `OwningArray<T>.Sort()` below on the same row. Its two other rows are
// `OwningBuffer<T>.DangerousPointer()` (§4.14) and `Result<T, E>`'s four value-payload
// combinators (§2.20), which take the identical shape. The register is closed and
// grows only by recorded ruling.
//
// `OwningArray<T>` has the same parameterless `Sort()`, declared in its own block
// below. It was documented from v0.242 to v0.273 and removed by the same v0.274
// ruling this one reverses -- a two-type deletion of which v0.275 undid only half.
// Three sema sites admit it positively (the §22.5 owning-container allow-list names
// `Sort`; the result-typing arm names `OwningArray` explicitly; the sole rejection
// arm covers `Reversed`/`Sorted` and omits `Sort`), and codegen reaches it because
// `OwningArray` is array-backed. NOTE: the `T: IComparable<T>` gate is `Array`-only
// in the compiler today, so an owning array of a class without `CompareTo` is
// admitted where it should be rejected -- #3325, the owning-side twin of #2790.
