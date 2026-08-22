public record ReadOnlyArraySpan<T>(Source: Array<T>, Start: int, Length: int): IReadOnlySpan<T>

  public property [i: int]: T
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Source[Start + i];
    end;
  end;

  public property IsEmpty: bool => Length = 0;

  public method Slice(start: int, length: int): ReadOnlyArraySpan<T> =>
    new ReadOnlyArraySpan<T>(Source, this.Start + start, length);
  public method Slice(start: int): ReadOnlyArraySpan<T> => Slice(start, Length - start);   // v0.195
  public method Slice(r: Range<int>): ReadOnlyArraySpan<T>;   // v0.241 — Range form: normalized against
                                                               //   Length (spec §16.3), then equivalent to
                                                               //   Slice(start, length); §4.14's contract

  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  public method SequenceEqual(other: ReadOnlyArraySpan<T>): bool;
  public method SequenceEqual(other: ArraySpan<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T>): bool; // v0.108 — interface form

  public method CopyTo(destination: ArraySpan<T>);            // v0.109 — copy slot-by-slot to destination;
                                                               //   lengths must match
end ReadOnlyArraySpan;
