public class Task
  // For non-class T (value types, string, runtime-managed reference types).
  // For class T, use FromOwnedResult instead.
  public static method FromResult<T>(value: T): Task<T>;

  // For class T. Produces a Pattern B task; awaiters consume under `using var x := await ...`.
  public static method FromOwnedResult<T>(owned value: T): OwnedTask<T> where T: class;

  // For any T: a faulted task never produces its result, so the Pattern A vs
  // Pattern B distinction does not apply. `FromException<MyClass>(ex)` is correctly
  // typed as Task<MyClass> even when MyClass is a class type — there is no value
  // to transfer ownership of, only an exception to re-raise on await.
  public static method FromException<T>(exception: Exception): Task<T>;
  public static method FromCancelled<T>(): Task<T>;

  public static method Delay(milliseconds: int): Task;
  public static method Delay(milliseconds: int, token: CancellationToken): Task;
  public static method Delay(duration: TimeSpan): Task;
  public static method Delay(duration: TimeSpan, token: CancellationToken): Task;

  public static method WhenAll(tasks: Array<Task>): Task;

  // For non-class T (value types, string, runtime-managed reference types).
  public static method WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>;

  // For class T. Aggregates OwnedTask<T> results into an owned array of owned elements.
  // The caller transfers ownership of the input array (and its tasks) to WhenAllOwned;
  // the returned task produces an owned OwnedArray<T>.
  public static method WhenAllOwned<T>(
    owned tasks: OwnedArray<OwnedTask<T>>
  ): OwnedTask<OwnedArray<T>> where T: class;

  public static method WhenAny(tasks: Array<Task>): Task<Task>;
  public static method FireAndForget(owned task: Task);
end Task;

Task.FireAndForget(DoTelemetryAsync());      // supervised; exceptions logged
using var t := DoTelemetryAsync();           // unsupervised; disposed at scope end
