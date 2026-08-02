public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>     // alias: string
  public readonly property Length: int;            // number of code points (O(1))
  public readonly property IsAscii: bool;          // true iff every code point < 128 (O(1))
  public readonly property [i: int]: char;                  // O(1)

  // Construction
  public static method From(value: char): string;
  public static method From(buffer: Array<char>, start: int, count: int): string;
  public static method From(value: CString): string;                  // FFI-boundary, owned-instance source
  public static method From(value: WString): string;                  // FFI-boundary, owned-instance source
  public static method FromCStringPointer(p: IntPtr): string;         // FFI-boundary, raw-pointer source
  public static method FromWStringPointer(p: IntPtr): string;         // FFI-boundary, raw-pointer source
  public static method Repeat(c: char, count: int): string;
  public static method Join(separator: string, items: IEnumerable<string>): string;

  // Inspection
  public static method IsNullOrEmpty(value: string?): bool;
  public static method IsNullOrWhiteSpace(value: string?): bool;
  public method Contains(value: string): bool;
  public method Contains(value: char): bool;
  public method StartsWith(value: string): bool;
  public method EndsWith(value: string): bool;
  public method IndexOf(value: char): int;
  public method IndexOf(value: string): int;
  public method LastIndexOf(value: char): int;

  // Substring
  public method Substring(startIndex: int): string;
  public method Substring(startIndex: int, length: int): string;
  public readonly property [r: Range<int>]: string;       // slicing form (§4.6 of the language spec)

  // Span views (v0.104)
  public method AsSpan(): ReadOnlyStringSpan;             // full string as a span
  public method AsSpan(start: int): ReadOnlyStringSpan;   // from offset to end
  public method AsSpan(start: int, length: int): ReadOnlyStringSpan;
  public method AsSpan(r: Range<int>): ReadOnlyStringSpan; // parallels [r: Range<int>]: string

  // Modification (returns a new string; original unchanged)
  public method Trim(): string;
  public method Trim(chars: IEnumerable<char>): string;
  public method TrimStart(): string;
  public method TrimEnd(): string;
  public method ToUpper(): string;
  public method ToLower(): string;
  public method Replace(oldChar: char, newChar: char): string;
  public method Replace(oldValue: string, newValue: string): string;
  public method PadLeft(totalWidth: int): string;
  public method PadLeft(totalWidth: int, paddingChar: char): string;
  public method PadRight(totalWidth: int): string;
  public method PadRight(totalWidth: int, paddingChar: char): string;
  public method Insert(startIndex: int, value: string): string;
  public method Remove(startIndex: int): string;
  public method Remove(startIndex: int, count: int): string;

  // Splitting
  public method Split(separator: char): owned Array<string>;
  public method Split(separator: string): owned Array<string>;
  public method Split(separators: Array<char>): owned Array<string>;

  // Comparison and equality.
  // All comparison and equality methods in this version are ordinal (code-point-
  // based), including the `ignoreCase` overloads — case-insensitive comparison
  // compares each code point's simple Unicode case-folded form, with no locale
  // or culture-specific tailoring. Culture-aware comparison will be provided by
  // System.Globalization in a future revision.
  public method CompareTo(other: string): int;            // ordinal
  public static method Compare(a: string, b: string): int;
  public static method Compare(a: string, b: string, ignoreCase: bool): int;
  public method Equals(other: string?): bool;             // satisfies IEquatable<string>'s
                                                          //   Equals(string) under §11.2's
                                                          //   parameter-nullability widening
  public method Equals(other: string?, ignoreCase: bool): bool;

  // Object overrides
  public override method Equals(other: Object?): bool;    // content-based, not reference-based
  public override method GetHashCode(): int;              // content-based, agrees with Equals
  public override method ToString(): string;              // returns this

  // Conversion
  public method ToCharArray(): owned Array<char>;

  // Iteration
  public method GetEnumerator(): owned IEnumerator<char>;

  // Operators
  public static method operator +(left: string, right: string): string;
end String;
