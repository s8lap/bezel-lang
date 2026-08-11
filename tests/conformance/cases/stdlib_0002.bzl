public static class Convert
  public static method ToInt32(value: string): int;
  public static method ToInt64(value: string): long;
  public static method ToDouble(value: string): double;
  public static method ToDecimal(value: string): decimal;
  public static method ToBoolean(value: string): bool;

  public static method TryToInt32(value: string, out result: int): bool;
  public static method TryToInt64(value: string, out result: long): bool;
  public static method TryToDouble(value: string, out result: double): bool;
  public static method TryToDecimal(value: string, out result: decimal): bool;
  public static method TryToBoolean(value: string, out result: bool): bool;

  public static method ToString(value: int): string;
  public static method ToString(value: long): string;                          // v0.165: elision expanded
  public static method ToString(value: double): string;
  public static method ToString(value: decimal): string;
  public static method ToString(value: bool): string;
  public static method ToString(value: int, toBase: int): string;              // bases 2, 8, 10, 16; ArgumentException otherwise (v0.195)
  public static method ToString(value: long, toBase: int): string;
end Convert;
