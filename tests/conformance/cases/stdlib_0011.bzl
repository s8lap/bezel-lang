public static class BitConverter
  // float (32-bit) ↔ int bit pattern
  public static method FloatToInt32Bits(value: float): int;
  public static method Int32BitsToFloat(value: int): float;

  // double (64-bit) ↔ long bit pattern
  public static method DoubleToInt64Bits(value: double): long;
  public static method Int64BitsToDouble(value: long): double;
end BitConverter;
