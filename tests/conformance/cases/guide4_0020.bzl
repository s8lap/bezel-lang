public async method SyncAll(items: Array<string>,
                            token: CancellationToken := CancellationToken.None): Task
  for var item in items do
    token.ThrowIfCancellationRequested();     // check at each safe point
    await CopyOneAsync(item, token);          // and thread the token inward
  end;
end SyncAll;

using var cts := CancellationTokenSource.WithCancelAfter(TimeSpan.FromSeconds(30));
await SyncAll(paths, cts.Token);              // awaited inside the source's scope
