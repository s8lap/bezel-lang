_ := DoTelemetryAsync();                 // ERROR: Task is non-trivial-dispose
Task.FireAndForget(DoTelemetryAsync());  // OK — supervised

using var _ := new FileStream(path);     // OK — disposed at scope end
