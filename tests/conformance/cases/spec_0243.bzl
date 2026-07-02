public class Semaphore
  public async method AcquireAsync(): OwningTask<SemaphoreToken>;
end Semaphore;

// Caller:
using var token := await sem.AcquireAsync();
DoWork();
// token disposed at scope end -> semaphore released.
