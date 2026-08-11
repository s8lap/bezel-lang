/// Compiler-intrinsic; aliased by the keyword `decimal`.
public record Decimal: IComparable<decimal>, IEquatable<decimal>
  public static const MinValue: decimal;
  public static const MaxValue: decimal;
  public static const Zero: decimal;
  public static const One: decimal;

  public static pure method Parse(s: string): decimal;
  public static method TryParse(s: string, out result: decimal): bool;
  public static pure method ParseOrNull(s: string): decimal?;         // v0.231: null exactly where Parse(s)
                                                                      //   raises; otherwise the parsed value
  public static pure method Parse(s: ReadOnlyStringSpan): decimal;                 // v0.222 (#1810): span form
  public static method TryParse(s: ReadOnlyStringSpan, out result: decimal): bool;

  public pure method ToString(): string;
  public pure method ToString(format: string): string;
  public pure method CompareTo(other: decimal): int;
  public pure method Round(digits: int): decimal;    // banker's rounding (round-half-to-even), matching
                                                //   Math.Round (§2.2); digits outside 0..28:
                                                //   ArgumentOutOfRangeException (v0.195)
end Decimal;
