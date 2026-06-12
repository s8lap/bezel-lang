/// Compiler-intrinsic; aliased by the keyword `float`.
public record Float: IComparable<float>, IEquatable<float>
  public static const MinValue: float;
  public static const MaxValue: float;
  public static const Epsilon: float;
  public static const PositiveInfinity: float;
  public static const NegativeInfinity: float;
  public static const NaN: float;

  public static method IsNaN(value: float): bool;
  public static method IsInfinity(value: float): bool;
  public static method IsPositiveInfinity(value: float): bool;
  public static method IsNegativeInfinity(value: float): bool;
  public static method IsFinite(value: float): bool;

  public static method Parse(s: string): float;
  public static method TryParse(s: string, out result: float): bool;

  // Equality variants
  public static method IeeeEquals(a: float, b: float): bool;       // strict IEEE
  public static method BitwiseEquals(a: float, b: float): bool;    // bit-pattern equal

  public method ToString(): string;
  public method ToString(format: string): string;
  public method CompareTo(other: float): int;
end Float;
