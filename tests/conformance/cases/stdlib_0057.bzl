public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int): IReadOnlySpan<T?>
  // class T

  // Indexer (bounds-checked against span Length as of v0.105).
  public property [i: int]: T?
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Buffer[Start + i];
    end;
    set(owned) => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      Buffer[Start + i] := value;
    end;
  end;

  // Range query (v0.101).
  public property IsEmpty: bool => Length = 0;

  // Search (v0.101).
  public method IndexOf(value: T?): int;          // -1 if not found
  public method Contains(value: T?): bool;        // IndexOf(value) >= 0

  // Equality (v0.101).
  public method SequenceEqual(other: Span<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T?>): bool; // v0.108 — interface form

  // Range construction (v0.99 / v0.100).
  public method Slice(start: int, length: int): Span<T> =>
    new Span<T>(Buffer, this.Start + start, length);

  public method AsReadOnly(): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, Start, Length);

  // Bulk mutation (v0.101).
  public method Clear();                          // set every slot to null
                                                   //   (disposes previous contents)
  public method MoveFrom(source: Span<T>);        // move slot-by-slot from source;
                                                   //   source slots become null;
                                                   //   destination's previous
                                                   //   contents disposed;
                                                   //   lengths must match
  public method MoveTo(destination: Span<T>);     // v0.109 — symmetric with MoveFrom;
                                                   //   move slot-by-slot to destination;
                                                   //   source slots become null;
                                                   //   destination's previous
                                                   //   contents disposed;
                                                   //   lengths must match
  public method Reverse();                         // reverse slot ordering in place
end Span;
