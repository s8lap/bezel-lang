public abstract class Encoding
  public abstract method GetBytes(s: string): owned Array<byte>;
  public abstract method GetString(bytes: Array<byte>): string;
  public abstract method GetString(bytes: Array<byte>, index: int, count: int): string;
  public abstract readonly property Name: string;

  public static readonly property Utf8: Encoding;
  public static readonly property Utf16: Encoding;
  public static readonly property Utf32: Encoding;
  public static readonly property Ascii: Encoding;
end Encoding;
