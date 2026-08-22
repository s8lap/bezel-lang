public interface IOwningEnumerable<T>: IDisposable where T: class
  method GetOwningEnumerator(): owned IOwningEnumerator<T>;
end IOwningEnumerable;

public interface IOwningEnumerator<T>: IDisposable where T: class
  method TakeCurrent(): owned T;        // transfers ownership (v0.161)
  method MoveNext(): bool;
end IOwningEnumerator;
