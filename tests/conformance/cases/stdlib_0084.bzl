// For non-class T (value types, string, runtime-managed reference types).
// Instantiating with a class T is a compile error pointing to OwnedTaskCompletionSource<T>.
public class TaskCompletionSource<T>
  public constructor();
  public readonly property Task: Task<T>;

  public method SetResult(result: T);
  public method SetException(exception: Exception);
  public method SetCancelled();

  public method TrySetResult(result: T): bool;
  public method TrySetException(exception: Exception): bool;
  public method TrySetCancelled(): bool;
end TaskCompletionSource;

// For class T. Result is transferred via `owned` parameter; produced task is Pattern B.
public class OwnedTaskCompletionSource<T> where T: class
  public constructor();
  public readonly property Task: OwnedTask<T>;

  public method SetResult(owned result: T);
  public method SetException(exception: Exception);
  public method SetCancelled();

  public method TrySetResult(owned result: T): bool;
  public method TrySetException(exception: Exception): bool;
  public method TrySetCancelled(): bool;
end OwnedTaskCompletionSource;

// Non-generic form for void-completion tasks
public class TaskCompletionSource
  public constructor();
  public readonly property Task: Task;
  public method SetResult();
  public method SetException(exception: Exception);
  public method SetCancelled();
  public method TrySetResult(): bool;
  public method TrySetException(exception: Exception): bool;
  public method TrySetCancelled(): bool;
end TaskCompletionSource;
