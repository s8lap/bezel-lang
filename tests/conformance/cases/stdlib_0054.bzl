public interface IOwningAsyncEnumerable<T>: IAsyncDisposable where T: class
  method GetOwningAsyncEnumerator(token: CancellationToken := CancellationToken.None): owned IOwningAsyncEnumerator<T>;
end IOwningAsyncEnumerable;

public interface IOwningAsyncEnumerator<T>: IAsyncDisposable where T: class
  readonly property Current: owned T;   // destructive
  method MoveNextAsync(): Task<bool>;
end IOwningAsyncEnumerator;
