public interface IDisposable
  method Dispose();
end IDisposable;

public interface IAsyncDisposable
  method DisposeAsync(): Task;
end IAsyncDisposable;
