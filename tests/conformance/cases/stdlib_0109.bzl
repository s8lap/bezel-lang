public class Task
  // For non-class T (copyable value types, string, runtime-managed reference types).
  // For class T, use FromOwnedResult instead; a move-only value T is rejected
  // per spec §30.5.
  public static method FromResult<T>(value: T): Task<T>;

  // For class T. Produces a Pattern B task; awaiters consume under `using var x := await ...`.
  public static method FromOwnedResult<T>(owned value: T): OwningTask<T> where T: class;

  // For any T over which Task<T> is well-formed (spec §30.5 — move-only value T
  // is not): a faulted task never produces its result, so the Pattern A vs
  // Pattern B distinction does not apply. `FromException<MyClass>(ex)` is correctly
  // typed as Task<MyClass> even when MyClass is a class type — there is no value
  // to transfer ownership of, only an exception to re-raise on await.
  public static method FromException<T>(owned exception: Exception): Task<T>;   // v0.179: the task owns its fault (spec §30.9)
  public static method FromCancelled<T>(): Task<T>;
  // Non-generic forms — spec §30.10 lists both; rows restored here v0.195:
  public static method FromException(owned exception: Exception): Task;   // v0.179 fault ownership, as above
  public static method FromCancelled(): Task;

  public static method Delay(milliseconds: int): Task;
  public static method Delay(milliseconds: int, token: CancellationToken): Task;
  public static method Delay(duration: TimeSpan): Task;
  public static method Delay(duration: TimeSpan, token: CancellationToken): Task;

  public static method WhenAll(tasks: Array<Task>): Task;

  // For non-class T (value types, string, runtime-managed reference types).
  public static method WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>;

  // For class T: the owning WhenAll overload (v0.250 — the parameter type
  // distinguishes it; the call site is Task.WhenAll(move tasks)).
  // Aggregates OwningTask<T> results into an owned array of owned elements.
  // The caller transfers ownership of the input array (and its tasks);
  // the returned task produces an owned OwningArray<T>.
  public static method WhenAll<T>(
    owned tasks: OwningArray<OwningTask<T>>
  ): OwningTask<OwningArray<T>> where T: class;

  public static method WhenAny(tasks: Array<Task>): Task<int>;   // index of the first task to SETTLE — success, fault,
                                                                 //   or cancellation; never re-raises the winner's fault
                                                                 //   (inspect tasks[i]); empty array: ArgumentException
                                                                 //   synchronously (spec §30.10; v0.162/v0.179)
  public static method FireAndForget(owned task: Task);
end Task;

Task.FireAndForget(DoTelemetryAsync());      // supervised; exceptions logged
using var t := DoTelemetryAsync();           // unsupervised; disposed at scope end
