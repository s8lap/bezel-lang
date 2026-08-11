public interface IEnumerable<out T>
  method GetEnumerator(): owned IEnumerator<T>;
end IEnumerable;

public interface IEnumerator<out T>: IDisposable
  readonly property Current: T;         // non-destructive: borrow of current value
  method MoveNext(): bool;
end IEnumerator;
