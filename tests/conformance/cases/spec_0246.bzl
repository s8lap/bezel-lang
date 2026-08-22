public async method LoadAsync(path: string): Task<string>
  using var task := File.ReadAllTextAsync(path);   // stdlib §7.3
  return await task;
end LoadAsync;
