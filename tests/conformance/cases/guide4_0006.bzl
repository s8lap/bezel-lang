using var conn := await ConnectAsync(url);     // yours; disposed at scope end
await using var s := await OpenSessionAsync(); // await using for async-disposable results
