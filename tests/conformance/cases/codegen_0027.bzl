public class OwningBuffer<T>                    // class T: conforms to IEnumerable<T?>; value T: to nothing
  constructor(length: int);                     // class T: all slots null;  value T: all slots default(T)
  constructor(length: int, init: (int) -> owned T);      // value T: init is (int) -> T
  readonly property Length: int;                // immutable after construction
  property [i: int]: T? { get;  set(owned); }   // class T: set disposes previous, `move buf[i]` ADMITTED
  property [i: int]: T  { get;  set; }          // value T: `move buf[i]` NOT admitted
  method DangerousPointer(): Ptr<T>;            // value T only; NOT YET IMPLEMENTED (#1717)
  method AsSpan([start[, length]] | r: Range<int>): Span<T>;
  method AsReadOnlySpan([start[, length]] | r: Range<int>): ReadOnlySpan<T>;
end
public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)  // : IReadOnlySpan<T?> at class T
  property [i: int]: T? { get; set(owned); }    // value T: [i: int]: T { get; set; }
  property IsEmpty: bool;   method Slice(start, length) / Slice(start) / Slice(r: Range<int>): Span<T>;
  method IndexOf(value): int;  Contains(value): bool;  SequenceEqual(other): bool;  AsReadOnly(); Reverse();
  method Clear();                               // class T only: nulls every slot, disposing previous
  method MoveFrom(source: Span<T>);  MoveTo(destination: Span<T>);          // class T only
  method Fill(value: T);  CopyFrom(source: ReadOnlySpan<T>);  CopyTo(destination: Span<T>);  // value T only
end
public record ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)
  // read surface + Slice x3;  value T only: CopyTo(destination: Span<T>).  No ToArray() (v0.277)
end
public record ArraySpan<T>(Source: Array<T>, Start: int, Length: int): IReadOnlySpan<T>
  property [i: int]: T { get; set; }            // the setter stores a BORROW; `move` NOT admitted
  property IsEmpty: bool;  Slice x3;  AsReadOnly(): ReadOnlyArraySpan<T>;  IndexOf; Contains; SequenceEqual;
  Fill(value: T);  CopyFrom(source: ReadOnlyArraySpan<T>);  CopyTo(destination: ArraySpan<T>);  Reverse();
end
public record ReadOnlyArraySpan<T>(Source: Array<T>, Start: int, Length: int): IReadOnlySpan<T>
  // read surface + CopyTo(destination: ArraySpan<T>).  No ToArray() (neither span family has one)
end
public interface IReadOnlySpan<TElement>        // implemented by all five span types
  readonly property Length: int;   readonly property IsEmpty: bool;
  property [i: int]: TElement { get }
  method IndexOf(value: TElement): int;   Contains(value: TElement): bool;
  method SequenceEqual(other: IReadOnlySpan<TElement>): bool;
end                                             // `Slice` is deliberately NOT on the interface
