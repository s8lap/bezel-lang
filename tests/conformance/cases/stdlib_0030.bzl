public abstract class Encoding
  public abstract method GetBytes(s: string): owned Array<byte>;
  public abstract method GetString(bytes: Array<byte>): string;
  public abstract method GetString(bytes: Array<byte>, index: int, count: int): string;
  public abstract readonly property Name: string;

  public static readonly property UTF8: Encoding;
  public static readonly property UTF16: Encoding;
  public static readonly property UTF32: Encoding;
  public static readonly property ASCII: Encoding;
end Encoding;
