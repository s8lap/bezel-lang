using var r := await LoadResourceAsync("/etc/foo");
Process(r);
// r disposed at scope end
