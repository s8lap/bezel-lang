public record BitReader(Source: ReadOnlySpan<byte>, BitStart: int, BitLength: int)
  public method ReadBitMsbFirst(): bool
    if BitLength < 1 then
      throw new IndexOutOfRangeException();
    end;
    var k := BitStart;
    return (int(Source[k / 8]) & (1 << (7 - (k mod 8)))) <> 0;
  end ReadBitMsbFirst;
end BitReader;
