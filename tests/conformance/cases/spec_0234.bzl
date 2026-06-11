public class Task
  public readonly property IsCompleted: bool;
  public readonly property IsFaulted: bool;
  public readonly property IsCancelled: bool;

  public method Wait();

  destructor;       // runs continuation cleanup and disposes any
                    //   unconsumed `owned` result. Task is non-trivial-
                    //   dispose, which is why discard `_ := someAsyncCall();`
                    //   is rejected ([§17.13](#sec-the-discard-_)).
end Task;

public class Task<T>: Task
  public readonly property Result: T;
end Task;

public class OwnedTask<T>: Task where T: class
  // Owning task: result transfers ownership to the awaiter (Pattern B).
  // Single-consumer by type. Reading the Result property is rejected
  // because Result would require borrow-source rules that this class
  // does not provide; consume via `await` only.
end OwnedTask;
