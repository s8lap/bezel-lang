public class Mutex
  public constructor();
  public method Acquire(): owned Lock;
  public method TryAcquire(timeout: TimeSpan): owned Lock?;
  public async method AcquireAsync(token: CancellationToken := CancellationToken.None): OwningTask<Lock>;
  public async method TryAcquireAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwningTask<Lock?>;
end Mutex;

public class Lock
  // No public constructor; obtained from Mutex.Acquire / TryAcquire.
  destructor;       // releases the held lock
end Lock;
