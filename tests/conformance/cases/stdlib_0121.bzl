public async method OpenFiles(paths: Array<string>): IOwningAsyncEnumerable<FileStream>
  for var path in paths do
    yield return await OpenFileAsync(path);
  end;
end OpenFiles;

// Consumer takes ownership of each FileStream:
public async method ProcessAll(paths: Array<string>): Task
  await for var file in OpenFiles(paths) do
    using var f := move file;  // discharge on entry — covers all exit edges (transfer marker)
    await ProcessAsync(f);
  end;
end ProcessAll;
