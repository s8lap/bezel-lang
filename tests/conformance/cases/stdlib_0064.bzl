public record ArraySpan<T>(Source: Array<T>, Start: int, Length: int): IReadOnlySpan<T>

  public property [i: int]: T
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Source[Start + i];
    end;
    set => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      Source[Start + i] := value;
    end;
  end;

  public readonly property IsEmpty: bool => Length = 0;

  // Range construction
  public method Slice(start: int, length: int): ArraySpan<T> =>
    new ArraySpan<T>(Source, this.Start + start, length);

  public method AsReadOnly(): ReadOnlyArraySpan<T> =>
    new ReadOnlyArraySpan<T>(Source, Start, Length);

  // Search
  public method IndexOf(value: T): int;
  public method Contains(value: T): bool;

  // Equality
  public method SequenceEqual(other: ArraySpan<T>): bool;
  public method SequenceEqual(other: ReadOnlyArraySpan<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T>): bool; // v0.108 — interface form

  // Bulk mutation
  public method Fill(value: T);                              // copy `value` into every slot
  public method CopyFrom(source: ReadOnlyArraySpan<T>);      // copy slot-by-slot; lengths must match
  public method CopyTo(destination: ArraySpan<T>);           // v0.109 — copy slot-by-slot to destination;
                                                              //   lengths must match
  public method Reverse();                                    // reverse slot ordering in place
end ArraySpan;
