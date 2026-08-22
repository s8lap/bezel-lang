public class OwningBuffer<T>                              // no IEnumerable conformance
                                                          //   (value-typed; see "Iteration" below)
  public constructor(length: int);                       // all slots default(T)
                                                          //   requires T to have a default value
  public constructor(length: int, init: (int) -> T);     // init returns T (no `owned`)

  public pure readonly property Length: int;    // stored extent, immutable once
                                                //   allocated; pure per spec §22.15
                                                //   *Intrinsic purity*

  public property [i: int]: T
    get;                                                  // borrow of slot (value copy
                                                          //   for primitives at the use site)
    set;                                                  // overwrites slot
  end;

  // move buf[i] NOT admitted (values have no ownership; use plain read)

  // Foreign-memory access (v0.186; language spec §32.12) — admitted only for
  //   T in the language spec §32.12 FFI-pointee set; see prose. The result type
  //   lives in System.Interop: naming Ptr<T> requires that import, using the
  //   returned value does not (language spec §2.8.3 rule 2; v0.265).
  public method DangerousPointer(): Ptr<T>;

  // Span views (v0.109)
  public method AsSpan(): Span<T>;                          // full buffer, writable
  public method AsSpan(start: int): Span<T>;                // from offset to end
  public method AsSpan(start: int, length: int): Span<T>;
  public method AsSpan(r: Range<int>): Span<T>;             // parallels Range slicing

  public method AsReadOnlySpan(): ReadOnlySpan<T>;
  public method AsReadOnlySpan(start: int): ReadOnlySpan<T>;
  public method AsReadOnlySpan(start: int, length: int): ReadOnlySpan<T>;
  public method AsReadOnlySpan(r: Range<int>): ReadOnlySpan<T>;
end OwningBuffer;
