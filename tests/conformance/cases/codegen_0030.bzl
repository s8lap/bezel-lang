public record CancellationToken                       // auto-imported; borrow-bearing
  public static readonly property None: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method ThrowIfCancellationRequested();
  public method Register(scoped callback: () -> ()): owned Subscription;
end
public class CancellationTokenSource                   // auto-imported
  public constructor();
  public static method WithCancelAfter(delay: TimeSpan): owned CancellationTokenSource;
  public static method CreateLinked(a: CancellationToken, b: CancellationToken): owned CancellationTokenSource;
  public static method CreateLinkedAll(parents: Array<CancellationToken>): owned CancellationTokenSource;
  public readonly property Token: CancellationToken;   IsCancellationRequested: bool;
  public method Cancel();   public method CancelAfter(delay: TimeSpan);
  destructor;                                          // cancel-on-dispose
end
public class Mutex
  constructor();
  method Acquire(): owned Lock;                        // BLOCKS: compile error inside an async body
  method TryAcquire(timeout: TimeSpan): owned Lock?;
  async method AcquireAsync(token := CancellationToken.None): OwningTask<Lock>;
  async method TryAcquireAsync(timeout: TimeSpan, token := CancellationToken.None): OwningTask<Lock?>;
end
public class Lock  destructor;  end                    // no public ctor; NOT sendable
public class Semaphore
  constructor(initialCount: int);   constructor(initialCount: int, maxCount: int);
  method Acquire(): owned SemaphoreToken;   TryAcquire(timeout): owned SemaphoreToken?;
  async method AcquireAsync(token := ...): OwningTask<SemaphoreToken>;
  async method TryAcquireAsync(timeout, token := ...): OwningTask<SemaphoreToken?>;
end
public class Atomic<T> where T: value            // T in {bool char byte sbyte short ushort int uint
  constructor(initial: T);   property Value: T { get; set; }      //     long ulong IntPtr UIntPtr}
  method Exchange(value: T): T;   method CompareExchange(expected: T, replacement: T): T;
end
public class AtomicInteger<T> where T: value     // the primitive INTEGER types only
  constructor(initial: T);   property Value: T { get; set; }
  method Exchange(value: T): T;   CompareExchange(expected, replacement): T;
  method Increment(): T;   Decrement(): T;   Add(delta: T): T;    // POST-operation value; OverflowException
end
// Events are a PATTERN, not a member kind: an `owned readonly` source field + a property exposing the
// interface.  IEvent<out T> / IAsyncEvent<out T> / ISharedEvent<out T> declare
//   method Subscribe(scoped handler: (T) -> () | (T) -> Task): owned Subscription;
// implemented by EventSource<T> / AsyncEventSource<T> (RaiseAsync) / SharedEventSource<T> (capture-free
// handlers only); each source adds Raise(payload: T), Add(scoped handler), Remove(handler), destructor.
public class TaskCompletionSource<T>                   // NON-class T; class T is a compile error
  constructor();   readonly property Task: Task<T>;    // the property is a BORROW of the source's storage
  method SetResult(result: T);   SetException(owned exception: Exception);   SetCancelled();
  method TrySetResult(result: T): bool;  TrySetException(owned ...): bool;  TrySetCancelled(): bool;
end                     // + OwningTaskCompletionSource<T> where T: class (SetResult(owned result: T),
                        //   Task: OwningTask<T>) and the void-completion TaskCompletionSource
public class Task
  static method FromResult<T>(value: T): Task<T>;                        // NON-class T
  static method FromOwnedResult<T>(owned value: T): OwningTask<T> where T: class;
  static method FromException<T>(owned exception: Exception): Task<T>;   FromCancelled<T>(): Task<T>;
  static method FromException(owned exception: Exception): Task;         FromCancelled(): Task;
  static method Delay(milliseconds: int[, token]): Task;   Delay(duration: TimeSpan[, token]): Task;
  static method WhenAll(tasks: Array<Task>): Task;
  static method WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>;                        // non-class T
  static method WhenAll<T>(owned tasks: OwningArray<OwningTask<T>>): OwningTask<OwningArray<T>> where T: class;
  static method WhenAny(tasks: Array<Task>): Task<int>;      // index of the first to SETTLE; never re-raises
  static method WhenEach(tasks: Array<Task>): IAsyncEnumerable<Task>;
  static method WhenEach<T>(tasks: Array<Task<T>>): IAsyncEnumerable<Task<T>>;
  static method WhenEach<T>(owned tasks: OwningArray<OwningTask<T>>): IOwningAsyncEnumerable<OwningTask<T>>
                                                                                          where T: class;
  static method FireAndForget(owned task: Task);
end
public class TaskScope: IAsyncDisposable                 // `await using` ONLY; not sendable
  constructor();
  readonly property Token: CancellationToken;
  method CancelAfter(delay: TimeSpan);   method Cancel();
  method Spawn(scoped body: () -> Task);                 // InvalidOperationException once closed
  async method Join(): Task;                             // MANDATORY on the normal path; first-fault re-raise
end
public class Channel<T> where T: sendable                // the RECEIVING end; single-consumer, MPSC
  constructor();                     // unbounded
  constructor(capacity: int);        // bounded, capacity >= 1 else ArgumentException
  readonly property Count: int;   IsClosed: bool;
  method NewSender(): owned ChannelSender<T>;   method Close();     // idempotent graceful close
  method Receive(): OwningTask<T>;                     // value/string T: Task<T>
  method TryReceive(out owned item: T?): bool;
  method ReceiveAll(): IOwningAsyncEnumerable<T>;      // value/string T: IAsyncEnumerable<T>
  destructor;
end
public class ChannelSender<T> where T: sendable
  method Send(owned item: T);                          // NEVER blocks; full bounded -> ChannelFullException
  async method SendAsync(owned item: T): Task;         // awaits capacity
  destructor;                                          // disposing the LAST sender closes the channel
end
public class Locked<T> where T: class                  // T must additionally be a SENDABLE class; Locked/RwLocked NOT YET IMPLEMENTED (#1721)
  constructor(owned value: T);                         // the lock OWNS the interior
  method Lock(): owned LockGuard<T>;   TryLock(timeout: TimeSpan): owned LockGuard<T>?;
  async method LockAsync(token := ...): OwningTask<LockGuard<T>>;
  async method TryLockAsync(timeout, token := ...): OwningTask<LockGuard<T>?>;
end
public class LockGuard<T>   public readonly property Value: T;   end     // borrow; source = this guard
public class RwLocked<T> where T: class                // Read/TryRead/ReadAsync/TryReadAsync +
  constructor(owned value: T);                         //   Write/TryWrite/WriteAsync/TryWriteAsync
end                                                    //   yielding ReadGuard<T> / WriteGuard<T>
public static class Domain            // 18 Spawn rows: {no arg, `owned arg: A`, `arg: A`} x {no result,
  Spawn(entry: () -> ()): owned Task;   //   `owned R` class result, `R` value result} x {sync, async entry}
  Spawn<A>(entry: (owned A) -> (), owned arg: A): owned Task    where A: sendable, class;
  Spawn<A>(entry: (A) -> (), arg: A): owned Task                where A: sendable, notnull;
  Spawn<R>(entry: () -> owned R): owned OwningTask<R>           where R: sendable, class;
  Spawn<R>(entry: () -> R): owned Task<R>                       where R: sendable, notnull;
end                                   // the entry MUST be capture-free; EXACTLY ONE argument
public static class Parallel          // callbacks are SPAWN POSITIONS (capture-free); NOT YET IMPLEMENTED (#3557)
  public static readonly property DefaultDegree: int;    // = Environment.ProcessorCount; degree := 0 means it
  Map<A, R>(items: Array<A>, body: (A) -> R, degree: int := 0): owned Task<Array<R>> where A, R: notnull;
  Reduce<A, R>(items: Array<A>, seed: R, selector: (A) -> R, combiner: (R, R) -> R,
               degree: int := 0): owned Task<R>                                      where A, R: notnull;
  ForEach<A>(items: Array<A>, body: (A) -> (), degree: int := 0): owned Task          where A: notnull;
  Map<A, R>(owned items: OwningArray<A>, body: (owned A) -> owned R,
            degree := 0): owned OwningTask<OwningArray<R>>                            where A, R: class;
  ForEach<A>(owned items: OwningArray<A>, body: (owned A) -> (), degree := 0): owned Task where A: class;
  // + the same three over Shared<FrozenArray<A>> (zero-copy)
end
