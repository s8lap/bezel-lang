await using var scope := new TaskScope();
private static owned readonly _ready: Atomic<bool> := new Atomic<bool>(false);
private static _cfg: Task<Config>? := null;               // the admitted Task-cache form
using var g := await mutex.AcquireAsync();
