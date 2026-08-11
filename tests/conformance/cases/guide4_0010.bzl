public async method RefreshBoth(primary: string, mirror: string): Task
  await using var scope := new TaskScope();
  scope.CancelAfter(TimeSpan.FromSeconds(30));           // deadline for the whole scope
  scope.Spawn(async lambda() => do
    await RefreshOneAsync(primary, scope.Token);         // children observe the scope's token
  end);
  scope.Spawn(async lambda() => do
    await RefreshOneAsync(mirror, scope.Token);
  end);
  await scope.Join();      // mandatory on the normal path: awaits all children,
end RefreshBoth;           //   re-raises the FIRST fault (siblings cancelled)
