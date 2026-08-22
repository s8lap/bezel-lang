var tasks := new Array<Task<string>>();
tasks.Add(File.ReadAllTextAsync(path));    // compiles, but WARNS: the fresh task has no holder
var conns: Array<Connection> := [new Connection("a")];   // ERROR: hard rejection at the literal site
_ := DoTelemetryAsync();                                 // ERROR: a Task may not be discarded
