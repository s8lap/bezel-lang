public static class BitConverter
  // float (32-bit) ↔ int bit pattern
  public static method FloatToInt32Bits(value: float): int;
  public static method Int32BitsToFloat(value: int): float;

  // double (64-bit) ↔ long bit pattern
  public static method DoubleToInt64Bits(value: double): long;
  public static method Int64BitsToDouble(value: long): double;

  // Same-width bit reinterpretation (compiler intrinsic). The single type
  // argument names the TARGET type; the source type is inferred from `value`.
  // The source and target must be primitive numeric types of equal bit width.
  public static method Reinterpret<TTo>(value: TFrom): TTo;
      // Pseudo-signature — compiler intrinsic: TFrom stands for the argument's
      //   inferred type, not a declared type parameter (not legal user Bezel as
      //   displayed). The primitive-numeric gate keeps every admissible
      //   instantiation value-typed, so the signature sits outside spec §28.3's
      //   bare-type-parameter rule (v0.195).
end BitConverter;
