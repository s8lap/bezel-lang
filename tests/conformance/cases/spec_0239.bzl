// Canonical form: using-var binding
using var token := await semaphore.AcquireAsync();
DoCriticalWork();
// At end of scope: token disposed, releasing the semaphore.

// Plain var binding — x is an owned local subject to standard discharge rules
var token := await semaphore.AcquireAsync();
TransferToHelper(token);    // owned local consumed by owned parameter

// Direct ownership transfer to a method call accepting owned
results.Add(await loadTask);     // results: OwnedArray<T>; Add takes owned

// Direct return
public async method Acquire(): OwnedTask<Connection>
  return await ConnectAsync(url);     // owned T flows to owned T return
end Acquire;
