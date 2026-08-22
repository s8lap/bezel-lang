/// Compiler-intrinsic; aliased by the keyword `float`.
public record Float: IComparable<float>, IEquatable<float>
  public static const MinValue: float;
  public static const MaxValue: float;
  public static const Epsilon: float;
  public static const PositiveInfinity: float;
  public static const NegativeInfinity: float;
  public static const NaN: float;

  public static pure method IsNaN(value: float): bool;
  public static pure method IsInfinity(value: float): bool;
  public static pure method IsPositiveInfinity(value: float): bool;
  public static pure method IsNegativeInfinity(value: float): bool;
  public static pure method IsFinite(value: float): bool;

  public static pure method Parse(s: string): float;
  public static method TryParse(s: string, out result: float): bool;
  public static pure method ParseOrNull(s: string): float?;           // v0.231: null exactly where Parse(s)
                                                                      //   raises; otherwise the parsed value
  public static pure method Parse(s: ReadOnlyStringSpan): float;                   // v0.222 (#1810): span form
  public static method TryParse(s: ReadOnlyStringSpan, out result: float): bool;

  // Equality variants
  public static pure method IeeeEquals(a: float, b: float): bool;       // strict IEEE
  public static pure method BitwiseEquals(a: float, b: float): bool;    // bit-pattern equal

  public pure method ToString(): string;
  public pure method ToString(format: string): string;
  public pure method CompareTo(other: float): int;
end Float;
