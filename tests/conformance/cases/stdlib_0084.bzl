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
