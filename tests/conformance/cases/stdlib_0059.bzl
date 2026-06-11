public record ReadOnlySpan<T>(Buffer: OwnedBuffer<T>, Start: int, Length: int): IReadOnlySpan<T?>
  // class T

  public property [i: int]: T?
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Buffer[Start + i];
    end;
  end;

  public readonly property IsEmpty: bool => Length = 0;

  public method IndexOf(value: T?): int;
  public method Contains(value: T?): bool;

  public method SequenceEqual(other: ReadOnlySpan<T>): bool;
  public method SequenceEqual(other: IReadOnlySpan<T?>): bool; // v0.108 — interface form

  public method Slice(start: int, length: int): ReadOnlySpan<T> =>
    new ReadOnlySpan<T>(Buffer, this.Start + start, length);
end ReadOnlySpan;
