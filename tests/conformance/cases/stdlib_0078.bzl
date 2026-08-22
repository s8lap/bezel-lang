public record BitReader(Source: ReadOnlySpan<byte>, BitStart: int, BitLength: int)

  public pure property IsEmpty: bool => BitLength = 0;
  public pure property IsByteAligned: bool => BitStart mod 8 = 0;

  // Reads — from bit 0 of the view; count in 1..64
  public method ReadBitsBigEndianMsbFirst(count: int): ulong;
  public method ReadBitsLittleEndianLsbFirst(count: int): ulong;
  public method ReadBitMsbFirst(): bool;
  public method ReadBitLsbFirst(): bool;

  // Try variants — return false instead of throwing on a short view
  public method TryReadBitsBigEndianMsbFirst(count: int, out value: ulong): bool;
  public method TryReadBitsLittleEndianLsbFirst(count: int, out value: ulong): bool;
  public method TryReadBitMsbFirst(out value: bool): bool;
  public method TryReadBitLsbFirst(out value: bool): bool;

  // Repositioning — the §4.14 Slice family, counted in bits
  public pure method Slice(bitStart: int): BitReader;
  public pure method Slice(bitStart: int, bitLength: int): BitReader;
  public pure method Slice(r: Range<int>): BitReader; // Range form: normalized against
                                                       //   BitLength (spec §16.3), then
                                                       //   equivalent to Slice(start, length)
  public pure method AlignToByte(): BitReader;        // advance to the next byte boundary

  // The byte-aligned remainder — an ordinary sub-span, no bit machinery
  public method AlignedTail(): ReadOnlySpan<byte>;
end BitReader;

public record BitWriter(Source: Span<byte>, BitStart: int, BitLength: int)

  public pure property IsEmpty: bool => BitLength = 0;
  public pure property IsByteAligned: bool => BitStart mod 8 = 0;

  public method WriteBitsBigEndianMsbFirst(value: ulong, count: int);
  public method WriteBitsLittleEndianLsbFirst(value: ulong, count: int);
  public method WriteBitMsbFirst(value: bool);
  public method WriteBitLsbFirst(value: bool);

  // Try variants — false on a short view; the destination is not modified
  public method TryWriteBitsBigEndianMsbFirst(value: ulong, count: int): bool;
  public method TryWriteBitsLittleEndianLsbFirst(value: ulong, count: int): bool;
  public method TryWriteBitMsbFirst(value: bool): bool;
  public method TryWriteBitLsbFirst(value: bool): bool;

  public pure method Slice(bitStart: int): BitWriter;
  public pure method Slice(bitStart: int, bitLength: int): BitWriter;
  public pure method Slice(r: Range<int>): BitWriter;
  public pure method AlignToByte(): BitWriter;

  public method AlignedTail(): Span<byte>;
  public method AsReadOnly(): BitReader;
end BitWriter;

// Entry points, parallel to AsSpan / AsReadOnlySpan
public extension ReadOnlySpan<byte>
  public pure method AsBitReader(): BitReader;
end;

public extension Span<byte>
  public method AsBitReader(): BitReader;             // delegates via AsReadOnly()
                                                       //   (unmarked), so not `pure`
  public pure method AsBitWriter(): BitWriter;
end;
