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
