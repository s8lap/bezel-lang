Task.FireAndForget(DoTelemetryAsync());      // supervised; exceptions logged
using var t := DoTelemetryAsync();           // unsupervised; disposed at scope end
