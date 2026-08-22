public class OwningTaskCompletionSource<T> where T: class
  public constructor();
  public readonly property Task: OwningTask<T>;

  public method SetResult(owned result: T);
  public method SetException(owned exception: Exception);   // the task owns its fault (§30.9)
  public method SetCancelled();

  public method TrySetResult(owned result: T): bool;
  public method TrySetException(owned exception: Exception): bool;
  public method TrySetCancelled(): bool;
end OwningTaskCompletionSource;
