public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>     // alias: string
  public pure readonly property Length: int;            // number of code points (O(1))
  public pure readonly property IsEmpty: bool;          // Length = 0 — §3.3's spelling (O(1)) (v0.231)
  public pure readonly property IsAscii: bool;          // true iff every code point < 128 (O(1))
  public pure readonly property [i: int]: char;                  // O(1)

  // Construction
  public static pure method From(value: char): string;
  public static pure method From(buffer: Array<char>, start: int, length: int): string;
  public static method From(value: CString): string;                  // FFI-boundary, owned-instance source
  public static method From(value: WString): string;                  // FFI-boundary, owned-instance source
  public static method FromCStringPointer(p: IntPtr): string;         // FFI-boundary, raw-pointer source
  public static method FromWStringPointer(p: IntPtr): string;         // FFI-boundary, raw-pointer source
  public static pure method Repeat(c: char, count: int): string;      // count < 0 raises
                                                                      //   ArgumentOutOfRangeException; count = 0 is "" (v0.215)
  public static pure method Repeat(unit: string, count: int): string; // v0.215: same count contract;
                                                                      //   Repeat("", n) = ""
  public static method Join(separator: string, items: IEnumerable<string>): string;

  // Inspection
  public static pure method IsNullOrEmpty(value: string?): bool;
  public static pure method IsNullOrWhiteSpace(value: string?): bool;
  public pure method Contains(value: string): bool;
  public pure method Contains(value: char): bool;
  public pure method StartsWith(value: string): bool;
  public pure method EndsWith(value: string): bool;
                              // Empty needle (v0.230): Contains("") = true; StartsWith("") = true;
                              //   EndsWith("") = true — the vacuous-match convention of IndexOf("") = 0;
                              //   the §3.3 span family mirrors the whole set.
  public pure method IndexOf(value: char): int;       // code-point index of first occurrence, or -1 (v0.195)
  public pure method IndexOf(value: string): int;     // -1 when absent; IndexOf("") = 0 (v0.195)
  public pure method IndexOf(value: char, startIndex: int): int;    // v0.215: search from the code-point
  public pure method IndexOf(value: string, startIndex: int): int;  //   index startIndex; 0 <= startIndex <= Length
                                                                    //   (IndexOutOfRangeException otherwise);
                                                                    //   IndexOf("", start) = start
  public pure method LastIndexOf(value: char): int;   // last occurrence, or -1
  public pure method LastIndexOf(value: string): int; // -1 when absent; LastIndexOf("") = Length (v0.195)

  // Substring
  public pure method Substring(startIndex: int): string;
  public pure method Substring(startIndex: int, length: int): string;
  public pure readonly property [r: Range<int>]: string;       // slicing form (§4.6 of the language spec)

  // Span views (v0.104)
  public pure method AsSpan(): ReadOnlyStringSpan;             // full string as a span
  public pure method AsSpan(start: int): ReadOnlyStringSpan;   // from offset to end
  public pure method AsSpan(start: int, length: int): ReadOnlyStringSpan;
  public pure method AsSpan(r: Range<int>): ReadOnlyStringSpan; // parallels [r: Range<int>]: string

  // Modification (returns a new string; original unchanged)
  public pure method Trim(): string;
  public pure method Trim(chars: IEnumerable<char>): string;
  public pure method TrimStart(): string;
  public pure method TrimEnd(): string;
  public pure method ToUpper(): string;
  public pure method ToLower(): string;
  public pure method Replace(oldChar: char, newChar: char): string;
  public pure method Replace(oldValue: string, newValue: string): string;
  public pure method PadLeft(totalWidth: int): string;
  public pure method PadLeft(totalWidth: int, paddingChar: char): string;
  public pure method PadRight(totalWidth: int): string;
  public pure method PadRight(totalWidth: int, paddingChar: char): string;
  public pure method PadCenter(totalWidth: int): string;                    // v0.215
  public pure method PadCenter(totalWidth: int, paddingChar: char): string; // v0.215
                              // Pad contract (v0.215, all five Pad forms): totalWidth <= Length returns the original string
                              // unchanged; totalWidth < 0 raises ArgumentOutOfRangeException.
                              // PadCenter splits the padding evenly; when it is odd, the extra
                              // character goes to the RIGHT: "ab".PadCenter(5) = " ab  ".

  public pure method Insert(startIndex: int, value: string): string;
  public pure method Remove(startIndex: int): string;
  public pure method Remove(startIndex: int, length: int): string;

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
  public pure method CompareTo(other: string): int;            // ordinal
  public static pure method Compare(a: string, b: string): int;
  public static pure method Compare(a: string, b: string, ignoreCase: bool): int;
  public pure method Equals(other: string?): bool;             // satisfies IEquatable<string>'s
                                                          //   Equals(string) under §11.2's
                                                          //   parameter-nullability widening
  public pure method Equals(other: string?, ignoreCase: bool): bool;

  // Object overrides
  public pure override method Equals(other: Object?): bool;    // content-based, not reference-based
  public pure override method GetHashCode(): int;              // content-based, agrees with Equals
  public pure override method ToString(): string;              // returns this

  // Conversion
  public method ToCharArray(): owned Array<char>;

  // Iteration
  public method GetEnumerator(): owned IEnumerator<char>;

  // Operators
  public static pure method operator +(left: string, right: string): string;
end String;
