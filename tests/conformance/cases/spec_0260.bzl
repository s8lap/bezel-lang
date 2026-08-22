public async method ReadLinesAsync(path: string): owned IAsyncEnumerable<string>
  // OpenAsync, ReadLineAsync, and AtEnd are illustrative; the example shows
  // the async-iterator shape, not a stream API in the standard library.
  // OpenAsync's result is a concrete class declaring a destructor, so `fs` may
  // be live across the `yield return` (the yield-liveness rule below).
  await using var fs := await OpenAsync(path);
  while not fs.AtEnd do
    yield return await fs.ReadLineAsync();
  end;
end ReadLinesAsync;
