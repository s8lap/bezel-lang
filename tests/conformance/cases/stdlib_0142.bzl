/// Compiler-intrinsic; aliased by the keyword `double`.
public record Double: IComparable<double>, IEquatable<double>
  public static const MinValue: double;
  public static const MaxValue: double;
  public static const Epsilon: double;
  public static const PositiveInfinity: double;
  public static const NegativeInfinity: double;
  public static const NaN: double;

  public static pure method IsNaN(value: double): bool;
  public static pure method IsInfinity(value: double): bool;
  public static pure method IsPositiveInfinity(value: double): bool;
  public static pure method IsNegativeInfinity(value: double): bool;
  public static pure method IsFinite(value: double): bool;

  public static pure method Parse(s: string): double;
  public static method TryParse(s: string, out result: double): bool;
  public static pure method ParseOrNull(s: string): double?;          // v0.231: null exactly where Parse(s)
                                                                      //   raises; otherwise the parsed value
  public static pure method Parse(s: ReadOnlyStringSpan): double;                  // v0.222 (#1810): span form
  public static method TryParse(s: ReadOnlyStringSpan, out result: double): bool;

  // Equality variants
  public static pure method IeeeEquals(a: double, b: double): bool;       // strict IEEE
  public static pure method BitwiseEquals(a: double, b: double): bool;    // bit-pattern equal

  public pure method ToString(): string;
  public pure method ToString(format: string): string;
  public pure method CompareTo(other: double): int;
end Double;
