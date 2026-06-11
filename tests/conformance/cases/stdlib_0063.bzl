extension ReadOnlySpan<byte>
  public method ReadInt32LittleEndian(): int
    if this.Length < 4 then
      throw new IndexOutOfRangeException();
    end;
    return int(this[0]) |
           (int(this[1]) << 8) |
           (int(this[2]) << 16) |
           (int(this[3]) << 24);
  end ReadInt32LittleEndian;
end;

extension Span<byte>
  public method WriteInt32BigEndian(value: int)
    if this.Length < 4 then
      throw new IndexOutOfRangeException();
    end;
    this[0] := byte((value >> 24) & 0xFF);
    this[1] := byte((value >> 16) & 0xFF);
    this[2] := byte((value >> 8) & 0xFF);
    this[3] := byte(value & 0xFF);
  end WriteInt32BigEndian;
end;
