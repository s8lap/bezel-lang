interface IAsyncEnumerable<out T>
  method GetAsyncEnumerator(token: CancellationToken): owned IAsyncEnumerator<T>;
end IAsyncEnumerable;

interface IAsyncEnumerator<out T>: IAsyncDisposable
  method MoveNextAsync(): Task<bool>;
  readonly property Current: T;
end IAsyncEnumerator;
