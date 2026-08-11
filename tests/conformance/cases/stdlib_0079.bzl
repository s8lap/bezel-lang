public interface IReadOnlySpan<TElement>
  readonly property Length: int;
  readonly property IsEmpty: bool;
  property [i: int]: TElement
    get;
  end;

  method IndexOf(value: TElement): int;
  method Contains(value: TElement): bool;
  method SequenceEqual(other: IReadOnlySpan<TElement>): bool;
end IReadOnlySpan;

public method Sum(s: ReadOnlySpan<int>): int;    // fast inner loop — direct dispatch
public method Sum(s: IReadOnlySpan<int>): int;   // convenient and broad — virtual dispatch
