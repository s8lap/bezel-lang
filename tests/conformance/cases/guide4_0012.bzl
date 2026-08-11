using var t1 := File.WriteAllTextAsync(pathA, report);   // both start now — named holders
using var t2 := File.WriteAllTextAsync(pathB, report);
var both: Array<Task> := [t1, t2];                       // borrows of the named holders
await Task.WhenAll(both);                                // settles ALL inputs; re-raises the
                                                         //   first non-successful input's fault
