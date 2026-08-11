using var task := FetchAsync(url);
var response := await task;              // borrowed reference, OK
ProcessResponse(response);
// At end of scope: task is disposed; response disposed with it.
