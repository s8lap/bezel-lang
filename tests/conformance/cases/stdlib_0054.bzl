public interface OwnedAsyncEnumerable<T>: IAsyncDisposable where T: class
  method GetOwnedAsyncEnumerator(token: CancellationToken := CancellationToken.None): owned OwnedAsyncEnumerator<T>;
end OwnedAsyncEnumerable;

public interface OwnedAsyncEnumerator<T>: IAsyncDisposable where T: class
  readonly property Current: owned T;   // destructive
  method MoveNextAsync(): Task<bool>;
end OwnedAsyncEnumerator;
