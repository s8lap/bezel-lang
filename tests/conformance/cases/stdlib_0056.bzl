public class OwnedBuffer<T>                              // no IEnumerable conformance
                                                          //   (value-typed; see "Iteration" below)
  public constructor(length: int);                       // all slots default(T)
                                                          //   requires T to have a default value
  public constructor(length: int, init: (int) -> T);     // init returns T (no `owned`)

  public readonly property Length: int;

  public property [i: int]: T
    get;                                                  // borrow of slot (value copy
                                                          //   for primitives at the use site)
    set;                                                  // overwrites slot
  end;

  // move buf[i] NOT admitted (values have no ownership; use plain read)

  // Span views (v0.109)
  public method AsSpan(): Span<T>;                          // full buffer, writable
  public method AsSpan(start: int): Span<T>;                // from offset to end
  public method AsSpan(start: int, length: int): Span<T>;
  public method AsSpan(r: Range<int>): Span<T>;             // parallels Range slicing

  public method AsReadOnlySpan(): ReadOnlySpan<T>;
  public method AsReadOnlySpan(start: int): ReadOnlySpan<T>;
  public method AsReadOnlySpan(start: int, length: int): ReadOnlySpan<T>;
  public method AsReadOnlySpan(r: Range<int>): ReadOnlySpan<T>;
end OwnedBuffer;
