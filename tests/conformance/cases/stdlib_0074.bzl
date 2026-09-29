public record ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int, Length: int): IReadOnlySpan<T>
  // value T

  public property [i: int]: T
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Buffer[Start + i];
    end;
  end;

  public property IsEmpty: bool => Length = 0;

  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  public method SequenceEqual(other: ReadOnlySpan<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T>): bool; // v0.108 — interface form

  public method Slice(start: int, length: int): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, this.Start + start, length);
  public method Slice(start: int): ReadOnlySpan<T> => Slice(start, Length - start);   // v0.195
  public method Slice(r: Range<int>): ReadOnlySpan<T>;   // v0.241 — Range form: normalized against
                                                          //   Length (spec §16.3), then equivalent to
                                                          //   Slice(start, length); see prose

  public method CopyTo(destination: Span<T>);     // v0.109 — copy slot-by-slot to destination;
                                                   //   lengths must match
end ReadOnlySpan;
