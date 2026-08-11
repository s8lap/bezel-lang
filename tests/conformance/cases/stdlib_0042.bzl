public record ReadOnlyStringSpan(Source: string, Start: int, Length: int): IReadOnlySpan<char>

  public property [i: int]: char
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Source[Start + i];
    end;
  end;

  public pure property IsEmpty: bool => Length = 0;

  // Range construction
  public pure method Slice(start: int, length: int): ReadOnlyStringSpan =>
    new ReadOnlyStringSpan(Source, this.Start + start, length);
  public pure method Slice(start: int): ReadOnlyStringSpan =>       // v0.195: from offset to end
    Slice(start, Length - start);
  public pure method Slice(r: Range<int>): ReadOnlyStringSpan;      // v0.241 — Range form: normalized
                                                                    //   against Length (spec §16.3), then
                                                                    //   equivalent to Slice(start, length);
                                                                    //   §4.14's normalization contract

  // Search by char
  public pure method IndexOf(value: char): int;
  public pure method Contains(value: char): bool;

  // Search by substring (span and string forms)
  public pure method IndexOf(value: ReadOnlyStringSpan): int;
  public pure method IndexOf(value: string): int;
  public pure method Contains(value: ReadOnlyStringSpan): bool;
  public pure method Contains(value: string): bool;

  // Prefix / suffix (span and string forms)
  public pure method StartsWith(prefix: ReadOnlyStringSpan): bool;
  public pure method StartsWith(prefix: string): bool;
  public pure method EndsWith(suffix: ReadOnlyStringSpan): bool;
  public pure method EndsWith(suffix: string): bool;

  // Equality (span and string forms)
  public pure method SequenceEqual(other: ReadOnlyStringSpan): bool;
  public pure method SequenceEqual(other: string): bool;
  public pure method SequenceEqual(other: IReadOnlySpan<char>): bool; // v0.108 — interface form

  // Bulk copy (v0.109)
  public method CopyTo(destination: Span<char>);    // copy chars to destination;
                                                     //   lengths must match, else
                                                     //   ArgumentException (the §4.14 rule)

  // Materialization
  public pure method ToString(): string;
end ReadOnlyStringSpan;
