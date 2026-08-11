/// Compiler-intrinsic; aliased by the keyword `char`.
public record Char: IComparable<char>, IEquatable<char>
  public static const MinValue: char;             // U+0000
  public static const MaxValue: char;             // U+10FFFF (excluding surrogates)

  public static pure method IsDigit(c: char): bool;
  public static pure method IsLetter(c: char): bool;
  public static pure method IsLetterOrDigit(c: char): bool;
  public static pure method IsWhiteSpace(c: char): bool;
  public static pure method IsUpper(c: char): bool;
  public static pure method IsLower(c: char): bool;
  public static pure method IsControl(c: char): bool;
  public static pure method IsPunctuation(c: char): bool;

  public static pure method ToUpper(c: char): char;
  public static pure method ToLower(c: char): char;

  public static pure method Parse(s: string): char;                 // exactly one code point
  public static method TryParse(s: string, out result: char): bool;
  public static pure method ParseOrNull(s: string): char?;          // v0.231: null exactly where Parse(s)
                                                                    //   raises; otherwise the parsed value

  public pure method ToString(): string;
  public pure method CompareTo(other: char): int;
end Char;
