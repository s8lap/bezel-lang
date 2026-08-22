// For non-class T (copyable value types, string, runtime-managed reference types).
// Instantiating with a class T is a compile error pointing to OwningTaskCompletionSource<T>;
// a move-only value T is rejected per spec §30.5 (its Task<T> is itself ill-formed).
public class TaskCompletionSource<T>
  public constructor();
  public readonly property Task: Task<T>;

  public method SetResult(result: T);
  public method SetException(owned exception: Exception);   // v0.179: the task owns its fault (spec §30.9)
  public method SetCancelled();

  public method TrySetResult(result: T): bool;
  public method TrySetException(owned exception: Exception): bool;
  public method TrySetCancelled(): bool;
  destructor;    // disposes the produced task, faulting it first if the source
                 //   is disposed before completion (spec §30.11, v0.184)
end TaskCompletionSource;

// For class T. Result is transferred via `owned` parameter; produced task is Pattern B.
public class OwningTaskCompletionSource<T> where T: class
  public constructor();
  public readonly property Task: OwningTask<T>;

  public method SetResult(owned result: T);
  public method SetException(owned exception: Exception);   // v0.179: the task owns its fault (spec §30.9)
  public method SetCancelled();

  public method TrySetResult(owned result: T): bool;
  public method TrySetException(owned exception: Exception): bool;
  public method TrySetCancelled(): bool;
  destructor;    // disposes the produced task, faulting it first if the source
                 //   is disposed before completion (spec §30.11, v0.184)
end OwningTaskCompletionSource;

// Non-generic form for void-completion tasks
public class TaskCompletionSource
  public constructor();
  public readonly property Task: Task;
  public method SetResult();
  public method SetException(owned exception: Exception);
  public method SetCancelled();
  public method TrySetResult(): bool;
  public method TrySetException(owned exception: Exception): bool;
  public method TrySetCancelled(): bool;
  destructor;    // disposes the produced task, faulting it first if the source
                 //   is disposed before completion (spec §30.11, v0.184)
end TaskCompletionSource;
