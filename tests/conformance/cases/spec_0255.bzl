var fetcher: (string) -> Task<string> := async lambda(url) => do
  using var client := new HttpClient();
  using var task := client.GetAsync(url);
  return await task;
end;

var quick: (string) -> Task<int> := async lambda(url) => await CountBytesAsync(url);

var loader: (string) -> OwningTask<Resource> := async lambda(path) => do
  return await LoadResourceAsync(path);     // Pattern B return; the lambda's
                                             //   state machine forwards the
                                             //   owned Resource through.
end;
