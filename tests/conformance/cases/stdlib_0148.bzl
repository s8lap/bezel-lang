/// Compiler-intrinsic; T restricted to integer types and char.
public record Range<T>: IEnumerable<T>
  public readonly property Start: T?;              // null if open-ended on the lower side
  public readonly property End: T?;                // null if open-ended on the upper side
  public readonly property Step: long;             // default 1; signed to support descending
  public readonly property IsEndInclusive: bool;   // true for `..`, false for `..<`
  public pure readonly property IsOpenEnded: bool; // true if Start or End is null
  public pure readonly property IsEmpty: bool;     // true if the range contains no values

  public pure method Contains(value: T): bool;
  public method GetEnumerator(): owned IEnumerator<T>;
end Range;
