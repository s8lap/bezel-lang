public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int): IReadOnlySpan<T>
  // value T (primitive, record, enum, value-like or borrow-bearing choice,
  //   non-move-only inline array — spec §15.3's copyable set)

  // Indexer (bounds-checked against span Length).
  public property [i: int]: T
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Buffer[Start + i];
    end;
    set => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      Buffer[Start + i] := value;
    end;
  end;

  // Range query.
  public property IsEmpty: bool => Length = 0;

  // Search.
  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  // Equality.
  public method SequenceEqual(other: Span<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T>): bool; // v0.108 — interface form

  // Range construction.
  public method Slice(start: int, length: int): Span<T> =>
    new Span<T>(Buffer, this.Start + start, length);
  public method Slice(start: int): Span<T> => Slice(start, Length - start);   // v0.195
  public method Slice(r: Range<int>): Span<T>;   // v0.241 — Range form: normalized against
                                                  //   Length (spec §16.3), then equivalent to
                                                  //   Slice(start, length); see prose

  public method AsReadOnly(): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, Start, Length);

  // Bulk mutation (value-typed forms).
  public method Fill(value: T);                   // copy `value` into every slot
  public method CopyFrom(source: ReadOnlySpan<T>); // copy slot-by-slot from source
                                                    //   lengths must match
  public method CopyTo(destination: Span<T>);      // v0.109 — copy slot-by-slot to destination
                                                    //   lengths must match
  public method Reverse();
end Span;
