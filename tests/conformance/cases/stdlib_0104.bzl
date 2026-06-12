/// Compiler-intrinsic; aliased by the keyword `decimal`.
public record Decimal: IComparable<decimal>, IEquatable<decimal>
  public static const MinValue: decimal;
  public static const MaxValue: decimal;
  public static const Zero: decimal;
  public static const One: decimal;

  public static method Parse(s: string): decimal;
  public static method TryParse(s: string, out result: decimal): bool;

  public method ToString(): string;
  public method ToString(format: string): string;
  public method CompareTo(other: decimal): int;
  public method Round(digits: int): decimal;
end Decimal;
