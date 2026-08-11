/// Compiler-intrinsic; aliased by the keyword `int`.
public record Int32: IComparable<int>, IEquatable<int>
  public static const MinValue: int := -2_147_483_648;
  public static const MaxValue: int := 2_147_483_647;

  public static pure method Parse(s: string): int;
  public static method TryParse(s: string, out result: int): bool;
  public static pure method ParseOrNull(s: string): int?;             // v0.231 (§1 *OrNull tier): null exactly
                                                                      //   where Parse(s) raises (the
                                                                      //   FormatException/OverflowException
                                                                      //   contract); otherwise the parsed
                                                                      //   value. Same strict input grammar as
                                                                      //   Parse — no whitespace tolerance
                                                                      //   (v0.137); string form only
  public static pure method Parse(s: string, radix: int): int;        // radix in {2, 8, 10, 16}; any other
                                                                      //   radix raises ArgumentException — a
                                                                      //   caller bug, not input failure (v0.228 pin)
  public static method TryParse(s: string, radix: int, out result: int): bool;
                                                                      // shape (c) (v0.228): false (out = 0)
                                                                      //   exactly where Parse(s, radix) raises
                                                                      //   FormatException/OverflowException;
                                                                      //   an invalid radix raises
                                                                      //   ArgumentException from both forms
  public static pure method Parse(s: ReadOnlyStringSpan): int;                     // v0.222 (#1810): identical contract
  public static method TryParse(s: ReadOnlyStringSpan, out result: int): bool;     //   to the string forms; shape (c)

  public pure method ToString(): string;
  public pure method ToString(format: string): string;
  public pure method ToString(radix: int): string;

  public pure method CompareTo(other: int): int;
  // Equals(other: int): bool — auto-generated from IEquatable<int>
  //   per language spec §9.2; not shown explicitly.
end Int32;
