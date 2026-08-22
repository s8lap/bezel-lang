public interface IEnumerable<out T>   method GetEnumerator(): owned IEnumerator<T>;  end
public interface IEnumerator<out T>: IDisposable
  readonly property Current: T;                      // non-destructive borrow
  method MoveNext(): bool;
end
public interface IAsyncEnumerable<out T>
  method GetAsyncEnumerator(token: CancellationToken := CancellationToken.None): owned IAsyncEnumerator<T>;
end
public interface IAsyncEnumerator<out T>: IAsyncDisposable
  readonly property Current: T;   method MoveNextAsync(): Task<bool>;
end
public interface IOwningEnumerable<T>: IDisposable where T: class
  method GetOwningEnumerator(): owned IOwningEnumerator<T>;
end
public interface IOwningEnumerator<T>: IDisposable where T: class
  method TakeCurrent(): owned T;   method MoveNext(): bool;
end
public interface IOwningAsyncEnumerable<T>: IAsyncDisposable where T: class
  method GetOwningAsyncEnumerator(token := CancellationToken.None): owned IOwningAsyncEnumerator<T>;
end
public interface IOwningAsyncEnumerator<T>: IAsyncDisposable where T: class
  method TakeCurrent(): owned T;   method MoveNextAsync(): Task<bool>;
end
