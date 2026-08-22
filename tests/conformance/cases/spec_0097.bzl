_ := DoTelemetryAsync();                 // ERROR: a Task may not be discarded — disposing
                                         //   a task abandons a running operation
Task.FireAndForget(DoTelemetryAsync());  // OK — supervised

using var _ := new FileStream(path, FileMode.Open);     // OK — disposed at scope end
