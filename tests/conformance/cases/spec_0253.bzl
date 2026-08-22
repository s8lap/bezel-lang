// Iteration variable from draining: task is owned, await consumes it
for var task in taskArray.Drain() do
  results.Add(await task);     // task discharged by the await; no further obligation
end;

// Local variable of owned task type
var theTask := SomeFactory();   // theTask: owned OwningTask<T>
using var result := await theTask;  // theTask discharged by the await
