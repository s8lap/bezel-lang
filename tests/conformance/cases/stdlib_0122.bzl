public class Locked<T> where T: class          // compiler-known; T must additionally be a
                                               //   sendable class type (checked at instantiation)
  public constructor(owned value: T);          // the lock owns the interior

  public method Lock(): owned LockGuard<T>;                          // blocks; §30.12 discipline
  public method TryLock(timeout: TimeSpan): owned LockGuard<T>?;     // null on timeout (Try shape (b))
  public async method LockAsync(token: CancellationToken := CancellationToken.None): OwningTask<LockGuard<T>>;
  public async method TryLockAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwningTask<LockGuard<T>?>;

  destructor;                                  // disposes the interior
end Locked;

public class LockGuard<T>
  // No public constructor; obtained from Locked<T>.
  public readonly property Value: T;           // borrow of the interior; source: this guard
  destructor;                                  // releases the lock
end LockGuard;

public class RwLocked<T> where T: class        // same interior admission as Locked<T>
  public constructor(owned value: T);

  public method Read(): owned ReadGuard<T>;                          // shared; blocks
  public method TryRead(timeout: TimeSpan): owned ReadGuard<T>?;
  public async method ReadAsync(token: CancellationToken := CancellationToken.None): OwningTask<ReadGuard<T>>;
  public async method TryReadAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwningTask<ReadGuard<T>?>;

  public method Write(): owned WriteGuard<T>;                        // exclusive; blocks
  public method TryWrite(timeout: TimeSpan): owned WriteGuard<T>?;
  public async method WriteAsync(token: CancellationToken := CancellationToken.None): OwningTask<WriteGuard<T>>;
  public async method TryWriteAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwningTask<WriteGuard<T>?>;

  destructor;                                  // disposes the interior
end RwLocked;

public class ReadGuard<T>
  public readonly property Value: T;           // borrow; WRITE-RESTRICTED (language spec §30.12)
  destructor;                                  // releases the shared hold
end ReadGuard;

public class WriteGuard<T>
  public readonly property Value: T;           // borrow; full access
  destructor;                                  // releases the exclusive hold
end WriteGuard;
