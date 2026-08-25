public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>
  pure readonly property Length: int;   IsEmpty: bool;   IsAscii: bool;
  pure readonly property [i: int]: char;      pure readonly property [r: Range<int>]: string;
  static pure From(value: char): string;   static pure From(buffer: Array<char>, start: int, length: int): string;
  static From(value: CString|WString): string;   static FromCStringPointer/FromWStringPointer(p: IntPtr): string;
  static pure Repeat(c: char, count: int): string;    static pure Repeat(unit: string, count: int): string;
  static Join(separator: string, items: IEnumerable<string>): string;
  static pure IsNullOrEmpty(value: string?): bool;    static pure IsNullOrWhiteSpace(value: string?): bool;
  pure Contains(string|char): bool;  pure StartsWith(string): bool;  pure EndsWith(string): bool;
  pure IndexOf(char|string[, startIndex: int]): int;   pure LastIndexOf(char|string): int;
  pure Substring(startIndex[, length]): string;
  pure AsSpan([start[, length]] | r: Range<int>): ReadOnlyStringSpan;
  pure Trim() / Trim(chars: IEnumerable<char>) / TrimStart() / TrimEnd() / ToUpper() / ToLower(): string;
  pure Replace(oldChar: char, newChar: char) / Replace(oldValue: string, newValue: string): string;
  pure PadLeft/PadRight/PadCenter(totalWidth: int[, paddingChar: char]): string;
  pure Insert(startIndex: int, value: string): string;   pure Remove(startIndex[, count]): string;
  method Split(separator: char): owned Array<string>;
  method Split(separator: string): owned Array<string>;
  method Split(separators: Array<char>): owned Array<string>;
  pure CompareTo(other: string): int;   static pure Compare(a, b[, ignoreCase: bool]): int;
  pure Equals(other: string?[, ignoreCase: bool]): bool;
  pure override Equals(other: Object?): bool;   pure override GetHashCode(): int;   pure override ToString(): string;
  method ToCharArray(): owned Array<char>;      method GetEnumerator(): owned IEnumerator<char>;
  static pure operator +(left: string, right: string): string;
end
public record Range<T>: IEnumerable<T>            // compiler-intrinsic; T = the integer types and char
  public readonly property Start: T?;   End: T?;   Step: long;   IsEndInclusive: bool;
  public pure readonly property IsOpenEnded: bool;      public pure readonly property IsEmpty: bool;
  public pure method Contains(value: T): bool;
  public method GetEnumerator(): owned IEnumerator<T>;      // NOT pure and cannot be
end
