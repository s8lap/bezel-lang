public record ReadOnlyStringSpan(Source: string, Start: int, Length: int): IReadOnlySpan<char>

  public property [i: int]: char
    get => do
      if i < 0 or i >= Length then
        throw new IndexOutOfRangeException();
      end;
      return Source[Start + i];
    end;
  end;

  public property IsEmpty: bool => Length = 0;

  // Range construction
  public method Slice(start: int, length: int): ReadOnlyStringSpan =>
    new ReadOnlyStringSpan(Source, this.Start + start, length);

  // Search by char
  public method IndexOf(value: char): int;
  public method Contains(value: char): bool;

  // Search by substring (span and string forms)
  public method IndexOf(value: ReadOnlyStringSpan): int;
  public method IndexOf(value: string): int;
  public method Contains(value: ReadOnlyStringSpan): bool;
  public method Contains(value: string): bool;

  // Prefix / suffix (span and string forms)
  public method StartsWith(prefix: ReadOnlyStringSpan): bool;
  public method StartsWith(prefix: string): bool;
  public method EndsWith(suffix: ReadOnlyStringSpan): bool;
  public method EndsWith(suffix: string): bool;

  // Equality (span and string forms)
  public method SequenceEqual(other: ReadOnlyStringSpan): bool;
  public method SequenceEqual(other: string): bool;
  public method SequenceEqual(other: IReadOnlySpan<char>): bool; // v0.108 — interface form

  // Bulk copy (v0.109)
  public method CopyTo(destination: Span<char>);    // copy chars to destination;
                                                     //   lengths must match

  // Materialization
  public method ToString(): string;
end ReadOnlyStringSpan;
