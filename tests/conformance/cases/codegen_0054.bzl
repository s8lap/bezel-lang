using var t1 := File.WriteAllTextAsync(pathA, report);   // named holders
using var t2 := File.WriteAllTextAsync(pathB, report);
var both: Array<Task> := [t1, t2];
await Task.WhenAll(both);
Task.FireAndForget(DoTelemetryAsync());
