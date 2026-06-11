public static class Convert
  public static method ToInt32(value: string): int;
  public static method ToInt64(value: string): long;
  public static method ToDouble(value: string): double;
  public static method ToDecimal(value: string): decimal;
  public static method ToBoolean(value: string): bool;

  public static method TryToInt32(value: string, out result: int): bool;
  // ... corresponding Try forms

  public static method ToString(value: int): string;
  public static method ToString(value: int, radix: int): string;   // 2, 8, 10, 16
  // ... corresponding ToString forms for other primitives
end Convert;
