var cfg := LoadAsync().Result;                            // ERROR inside async code: blocks its own domain
using var j := Domain.Spawn(lambda() => Index(repo));     // ERROR: a spawn entry must be CAPTURE-FREE
private static _cache: Dictionary<string, int> := new Dictionary<string, int>();   // ERROR: mutable static
using var scope: TaskScope := new TaskScope();            // ERROR: TaskScope needs `await using`
using var g := mutex.Acquire();                           // ERROR inside an async body: it blocks
