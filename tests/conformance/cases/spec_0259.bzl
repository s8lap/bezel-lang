var reader: (string) -> Task<string> := async lambda(path) => do
  using var task := File.ReadAllTextAsync(path);   // stdlib §7.3
  return await task;
end;

var quick: (string) -> Task<int> := async lambda(url) => await CountBytesAsync(url);

var loader: (string) -> OwningTask<Resource> := async lambda(path) => do
  return await LoadResourceAsync(path);     // Pattern B return; the lambda's
                                             //   state machine forwards the
                                             //   owned Resource through.
end;
