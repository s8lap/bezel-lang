/// Compiler-intrinsic; T restricted to integer types and char.
public record Range<T>: IEnumerable<T>
  public readonly property Start: T?;
  public readonly property End: T?;
  public readonly property Step: long;
  public readonly property IsEndInclusive: bool;
  public readonly property IsOpenEnded: bool;
  public readonly property IsEmpty: bool;

  public method Contains(value: T): bool;
  public method GetEnumerator(): owned IEnumerator<T>;
end Range;
