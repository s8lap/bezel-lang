interface IEnumerable<out T>
  method GetEnumerator(): owned IEnumerator<T>;
end IEnumerable;

interface IEnumerator<out T>: IDisposable
  method MoveNext(): bool;
  readonly property Current: T;
end IEnumerator;
