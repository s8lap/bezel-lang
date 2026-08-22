public class TaskCompletionSource<T>
  // Intrinsic restriction: T must be a copyable value type, string, or other
  // runtime-managed reference type. Instantiating with a class T is a
  // compile error pointing to OwningTaskCompletionSource<T>; a move-only
  // value T is rejected per §30.5 (its Task<T> is itself ill-formed).
  public constructor();
  public readonly property Task: Task<T>;

  public method SetResult(result: T);
  public method SetException(owned exception: Exception);   // the task owns its fault (§30.9)
  public method SetCancelled();

  public method TrySetResult(result: T): bool;
  public method TrySetException(owned exception: Exception): bool;
  public method TrySetCancelled(): bool;
end TaskCompletionSource;
