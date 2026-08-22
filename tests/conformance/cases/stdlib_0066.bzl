public interface IAsyncEnumerable<out T>
  method GetAsyncEnumerator(token: CancellationToken := CancellationToken.None): owned IAsyncEnumerator<T>;
end IAsyncEnumerable;

public interface IAsyncEnumerator<out T>: IAsyncDisposable
  readonly property Current: T;
  method MoveNextAsync(): Task<bool>;
end IAsyncEnumerator;
