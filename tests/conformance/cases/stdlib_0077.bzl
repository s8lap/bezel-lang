public class Semaphore
  public constructor(initialCount: int);
  public constructor(initialCount: int, maxCount: int);

  public method Acquire(): owned SemaphoreToken;
  public method TryAcquire(timeout: TimeSpan): owned SemaphoreToken?;
  public async method AcquireAsync(token: CancellationToken := CancellationToken.None): OwnedTask<SemaphoreToken>;
  public async method TryAcquireAsync(timeout: TimeSpan, token: CancellationToken := CancellationToken.None): OwnedTask<SemaphoreToken?>;
end Semaphore;

public class SemaphoreToken
  destructor;       // releases the held permit
end SemaphoreToken;
