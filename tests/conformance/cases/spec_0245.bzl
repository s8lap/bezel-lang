public async method ReadLinesAsync(path: string): IAsyncEnumerable<string>
  // OpenAsync, ReadLineAsync, and AtEnd are illustrative; the example shows
  // the async-iterator shape, not a stream API in the standard library.
  await using var fs := await OpenAsync(path);
  while not fs.AtEnd do
    yield return await fs.ReadLineAsync();
  end;
end ReadLinesAsync;
