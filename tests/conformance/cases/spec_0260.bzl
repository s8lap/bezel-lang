public async method OpenFilesAsync(paths: IEnumerable<string>): owned IOwningAsyncEnumerable<FileStream>
  for var path in paths do
    yield return await OpenAsync(path);    // each yielded FileStream is owned
  end;
end OpenFilesAsync;

public async method ProcessAllAsync(paths: IEnumerable<string>): Task
  await for var fs in OpenFilesAsync(paths) do
    using var f := move fs;                 // discharge owned iteration variable
    await ProcessOneAsync(f);
  end;
end ProcessAllAsync;
