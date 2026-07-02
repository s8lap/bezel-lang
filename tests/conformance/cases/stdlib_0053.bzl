public interface OwnedEnumerable<T>: IDisposable where T: class
  method GetOwnedEnumerator(): owned OwnedEnumerator<T>;
end OwnedEnumerable;

public interface OwnedEnumerator<T>: IDisposable where T: class
  method TakeCurrent(): owned T;        // transfers ownership (v0.161; was destructive property Current)
  method MoveNext(): bool;
end OwnedEnumerator;
