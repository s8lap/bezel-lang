/// Compiler-intrinsic; aliased by the keyword `int`.
public record Int32: IComparable<int>, IEquatable<int>
  public static const MinValue: int := -2_147_483_648;
  public static const MaxValue: int := 2_147_483_647;

  public static method Parse(s: string): int;
  public static method TryParse(s: string, out result: int): bool;
  public static method Parse(s: string, radix: int): int;        // 2, 8, 10, 16

  public method ToString(): string;
  public method ToString(format: string): string;
  public method ToString(radix: int): string;

  public method CompareTo(other: int): int;
  // Equals(other: int): bool — auto-generated from IEquatable<int>
  //   per language spec §9.2; not shown explicitly.
end Int32;
