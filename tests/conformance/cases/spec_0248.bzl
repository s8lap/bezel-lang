public class Task
  public readonly property IsCompleted: bool;
  public readonly property IsFaulted: bool;
  public readonly property IsCancelled: bool;

  public method Wait();

  destructor;       // runs continuation cleanup and disposes any
                    //   unconsumed `owned` result. Disposing a task
                    //   abandons a running operation, which is why
                    //   `_ := someAsyncCall();` is rejected
                    //   ([§17.13](#sec-the-discard-_), A6).
end Task;

public class Task<T>: Task
  public readonly property Result: T;
end Task;

public class OwningTask<T>: Task where T: class
  // Owning task: result transfers ownership to the awaiter (Pattern B).
  // Single-consumer by type.
  public readonly property Result: T;
                    // reading Result on a COMPLETED OwningTask
                    // in an owned-consuming position transfers ownership
                    // exactly like `await` (the result buffer is consumed;
                    // a second read/await raises InvalidOperationException).
                    // Subject to the same §30.3 blocking rules as Task<T>.Result:
                    // legal on an incomplete task only outside domains.
end OwningTask;
