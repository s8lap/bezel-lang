// Abbreviated from stdlib §6.3: the declared parameter list also carries
//   `token: CancellationToken := CancellationToken.None`.
public class Semaphore
  public async method AcquireAsync(): OwningTask<SemaphoreToken>;
end Semaphore;

// Caller:
using var token := await sem.AcquireAsync();
DoWork();
// token disposed at scope end -> semaphore released.
