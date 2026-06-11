public class OwnedBuffer<T>: IEnumerable<T?>            // class T only
  public constructor(length: int);                       // all slots null
  public constructor(length: int, init: (int) -> owned T);

  public readonly property Length: int;

  public property [i: int]: T?
    get;                                                  // borrow of slot
    set(owned);                                           // takes owned T?; disposes previous
  end;

  // move buf[i] admitted (returns owned T?, leaves slot moved-from;
  //   the field-restoration rule of language spec §28.8.1 applies)

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
