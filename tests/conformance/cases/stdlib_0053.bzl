public interface OwnedEnumerable<T>: IDisposable where T: class
  method GetOwnedEnumerator(): owned OwnedEnumerator<T>;
end OwnedEnumerable;

public interface OwnedEnumerator<T>: IDisposable where T: class
  readonly property Current: owned T;   // destructive: transfers ownership
  method MoveNext(): bool;
end OwnedEnumerator;
