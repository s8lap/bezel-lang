public class TaskCompletionSource<T>
  // Intrinsic restriction: T must be a value type, string, or other
  // runtime-managed reference type. Instantiating with a class T is a
  // compile error pointing to OwnedTaskCompletionSource<T>.
  public constructor();
  public readonly property Task: Task<T>;

  public method SetResult(result: T);
  public method SetException(exception: Exception);
  public method SetCancelled();

  public method TrySetResult(result: T): bool;
  public method TrySetException(exception: Exception): bool;
  public method TrySetCancelled(): bool;
end TaskCompletionSource;
