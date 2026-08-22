/// Compiler-intrinsic; T restricted to integer types and char.
public record Range<T>: IEnumerable<T>
  public readonly property Start: T?;
  public readonly property End: T?;
  public readonly property Step: long;
  public readonly property IsEndInclusive: bool;
  public pure readonly property IsOpenEnded: bool;
  public pure readonly property IsEmpty: bool;

  public pure method Contains(value: T): bool;
  public method GetEnumerator(): owned IEnumerator<T>;
end Range;
