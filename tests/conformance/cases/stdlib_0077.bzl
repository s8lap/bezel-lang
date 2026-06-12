using var guard := mutex.TryAcquire(TimeSpan.FromSeconds(5));
if guard <> null then
  // critical section; guard non-null in this branch (narrowing)
end;
// At scope end: guard disposed if non-null, releasing the lock; null is a no-op.
