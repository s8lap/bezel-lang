public async method ReadSectorAsync(dev: BlockDevice, lba: long): Task<int>
  using var tcs := new TaskCompletionSource<int>();

  dev.BeginRead(lba, lambda(status) => do        // BeginRead invokes the callback ONCE
    tcs.TrySetResult(status);
  end);

  return await tcs.Task;
end ReadSectorAsync;
