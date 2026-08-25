public abstract class Encoding
  public abstract method GetBytes(s: string): owned Array<byte>;
  public abstract method GetString(bytes: Array<byte>): string;
  public abstract method GetString(bytes: Array<byte>, start: int, length: int): string;
                              // window over Count; IndexOutOfRangeException out of range (v0.165)
  public abstract method GetString(bytes: ReadOnlySpan<byte>): string;       // v0.222 (#1809): buffer-backed source
  public abstract method GetString(bytes: ReadOnlyArraySpan<byte>): string;  // v0.222 (#1809): Array-backed twin (#1358)
  public abstract method GetByteCount(s: string): int;                       // v0.165 (#1355)
  public abstract method GetBytes(s: string, destination: Span<byte>): int;  // v0.165: returns bytes written;
                                                                             //   IndexOutOfRangeException if too short
  public abstract method GetByteCount(s: ReadOnlyStringSpan): int;           // span-source encode (v0.228):
  public abstract method GetBytes(s: ReadOnlyStringSpan,                     //   identical contract to the string
                                  destination: Span<byte>): int;             //   forms over the span's range
  public abstract method GetBytes(s: ReadOnlyStringSpan): owned Array<byte>; //   one-shot allocating form (v0.228)
  public abstract readonly property Name: string;

  public static readonly property UTF8: Encoding;
  public static readonly property UTF16: Encoding;
  public static readonly property UTF32: Encoding;
  public static readonly property ASCII: Encoding;
end Encoding;
