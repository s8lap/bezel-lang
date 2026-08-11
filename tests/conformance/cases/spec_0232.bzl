public async method ProcessAsync(): Task
  // ConnectAsync and SendAsync are illustrative: the standard library's
  // System.Net module is deferred to a future revision (std lib §11).
  await using var connection := await ConnectAsync(url);
  await connection.SendAsync(data);
  // connection.DisposeAsync() awaited at scope end
end ProcessAsync;
