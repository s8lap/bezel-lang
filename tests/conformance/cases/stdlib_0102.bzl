/// Compiler-intrinsic; aliased by the keyword `double`.
public record Double: IComparable<double>, IEquatable<double>
  public static const MinValue: double;
  public static const MaxValue: double;
  public static const Epsilon: double;
  public static const PositiveInfinity: double;
  public static const NegativeInfinity: double;
  public static const NaN: double;

  public static method IsNaN(value: double): bool;
  public static method IsInfinity(value: double): bool;
  public static method IsPositiveInfinity(value: double): bool;
  public static method IsNegativeInfinity(value: double): bool;
  public static method IsFinite(value: double): bool;

  public static method Parse(s: string): double;
  public static method TryParse(s: string, out result: double): bool;

  // Equality variants
  public static method IeeeEquals(a: double, b: double): bool;       // strict IEEE
  public static method BitwiseEquals(a: double, b: double): bool;    // bit-pattern equal

  public method ToString(): string;
  public method ToString(format: string): string;
  public method CompareTo(other: double): int;
end Double;
