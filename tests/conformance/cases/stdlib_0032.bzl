public record ReadOnlyAsciiByteSpan(Source: string, Start: int, Length: int): IReadOnlySpan<byte>

  public property [i: int]: byte
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return byte(Source[Start + i]);
    end;
  end;

  public property IsEmpty: bool => Length = 0;

  // Range construction
  public method Slice(start: int, length: int): ReadOnlyAsciiByteSpan =>
    new ReadOnlyAsciiByteSpan(Source, this.Start + start, length);

  // Search
  public method IndexOf(value: byte): int;
  public method Contains(value: byte): bool;

  // Equality (own type and cross-source against ReadOnlySpan<byte>)
  public method SequenceEqual(other: ReadOnlyAsciiByteSpan): bool;
  public method SequenceEqual(other: ReadOnlySpan<byte>): bool;
  public method SequenceEqual(other: IReadOnlySpan<byte>): bool; // v0.108 — interface form

  // Bulk copy (v0.109)
  public method CopyTo(destination: Span<byte>);    // copy bytes to destination;
                                                     //   lengths must match
end ReadOnlyAsciiByteSpan;
