using var task := File.ReadAllTextAsync(path);   // fresh task: yours, disposed at scope end
var text := await task;
