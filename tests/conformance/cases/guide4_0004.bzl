Task.FireAndForget(DoTelemetryAsync());   // supervised: runs to completion, exceptions logged
using var t := DoTelemetryAsync();        // unsupervised: disposed at scope end, without
                                          //   blocking; its exceptions are swallowed
