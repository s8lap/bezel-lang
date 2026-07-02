var fetcher: (string) -> Task<string> := async (url) => do
  using var client := new HttpClient();
  using var task := client.GetAsync(url);
  return await task;
end;

var quick: (string) -> Task<int> := async (url) => await CountBytesAsync(url);

var loader: (string) -> OwningTask<Resource> := async (path) => do
  return await LoadResourceAsync(path);     // Pattern B return; the lambda's
                                             //   state machine forwards the
                                             //   owned Resource through.
end;
