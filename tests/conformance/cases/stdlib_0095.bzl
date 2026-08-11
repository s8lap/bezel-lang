using var guard := await mutex.AcquireAsync();
// critical section; guard disposed at scope end.

using var maybe := await mutex.TryAcquireAsync(TimeSpan.FromSeconds(5));
if maybe <> null then
  // critical section
end;
