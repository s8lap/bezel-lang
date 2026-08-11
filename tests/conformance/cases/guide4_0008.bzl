using var task := FetchConfigAsync();   // Task<Config>: fan-out
var cfg  := await task;                 // borrow; source: task
var cfg2 := await task;                 // fine — same instance, multi-award
