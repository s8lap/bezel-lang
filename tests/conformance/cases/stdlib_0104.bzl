/// Compiler-intrinsic; aliased by the keyword `char`.
public record Char: IComparable<char>, IEquatable<char>
  public static const MinValue: char;             // U+0000
  public static const MaxValue: char;             // U+10FFFF (excluding surrogates)

  public static method IsDigit(c: char): bool;
  public static method IsLetter(c: char): bool;
  public static method IsLetterOrDigit(c: char): bool;
  public static method IsWhiteSpace(c: char): bool;
  public static method IsUpper(c: char): bool;
  public static method IsLower(c: char): bool;
  public static method IsControl(c: char): bool;
  public static method IsPunctuation(c: char): bool;

  public static method ToUpper(c: char): char;
  public static method ToLower(c: char): char;

  public method ToString(): string;
  public method CompareTo(other: char): int;
end Char;
