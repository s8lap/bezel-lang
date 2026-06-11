extension ReadOnlySpan<byte>
  // 16-bit integers
  public method ReadInt16LittleEndian(): short;
  public method ReadInt16BigEndian(): short;
  public method ReadUInt16LittleEndian(): ushort;
  public method ReadUInt16BigEndian(): ushort;

  // 32-bit integers
  public method ReadInt32LittleEndian(): int;
  public method ReadInt32BigEndian(): int;
  public method ReadUInt32LittleEndian(): uint;
  public method ReadUInt32BigEndian(): uint;

  // 64-bit integers
  public method ReadInt64LittleEndian(): long;
  public method ReadInt64BigEndian(): long;
  public method ReadUInt64LittleEndian(): ulong;
  public method ReadUInt64BigEndian(): ulong;

  // IEEE 754 floating point
  public method ReadFloatLittleEndian(): float;
  public method ReadFloatBigEndian(): float;
  public method ReadDoubleLittleEndian(): double;
  public method ReadDoubleBigEndian(): double;

  // Try variants — return false instead of throwing on short input
  public method TryReadInt16LittleEndian(out value: short): bool;
  public method TryReadInt16BigEndian(out value: short): bool;
  public method TryReadUInt16LittleEndian(out value: ushort): bool;
  public method TryReadUInt16BigEndian(out value: ushort): bool;
  public method TryReadInt32LittleEndian(out value: int): bool;
  public method TryReadInt32BigEndian(out value: int): bool;
  public method TryReadUInt32LittleEndian(out value: uint): bool;
  public method TryReadUInt32BigEndian(out value: uint): bool;
  public method TryReadInt64LittleEndian(out value: long): bool;
  public method TryReadInt64BigEndian(out value: long): bool;
  public method TryReadUInt64LittleEndian(out value: ulong): bool;
  public method TryReadUInt64BigEndian(out value: ulong): bool;
  public method TryReadFloatLittleEndian(out value: float): bool;
  public method TryReadFloatBigEndian(out value: float): bool;
  public method TryReadDoubleLittleEndian(out value: double): bool;
  public method TryReadDoubleBigEndian(out value: double): bool;
end;

extension Span<byte>
  // Reads and TryReads (delegate to AsReadOnly)
  public method ReadInt16LittleEndian(): short =>
    this.AsReadOnly().ReadInt16LittleEndian();
  public method ReadInt16BigEndian(): short =>
    this.AsReadOnly().ReadInt16BigEndian();
  // ... (full set of 16 Read methods and 16 TryRead methods, each
  //      delegating identically to the corresponding AsReadOnly method;
  //      omitted here for brevity)

  // Writes — Span<byte> only
  public method WriteInt16LittleEndian(value: short);
  public method WriteInt16BigEndian(value: short);
  public method WriteUInt16LittleEndian(value: ushort);
  public method WriteUInt16BigEndian(value: ushort);
  public method WriteInt32LittleEndian(value: int);
  public method WriteInt32BigEndian(value: int);
  public method WriteUInt32LittleEndian(value: uint);
  public method WriteUInt32BigEndian(value: uint);
  public method WriteInt64LittleEndian(value: long);
  public method WriteInt64BigEndian(value: long);
  public method WriteUInt64LittleEndian(value: ulong);
  public method WriteUInt64BigEndian(value: ulong);
  public method WriteFloatLittleEndian(value: float);
  public method WriteFloatBigEndian(value: float);
  public method WriteDoubleLittleEndian(value: double);
  public method WriteDoubleBigEndian(value: double);

  // Try variants — return false instead of throwing on short output buffer
  public method TryWriteInt16LittleEndian(value: short): bool;
  public method TryWriteInt16BigEndian(value: short): bool;
  // ... (full set of 16 TryWrite methods; omitted for brevity)
end;
