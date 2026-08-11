public async method LoadAllConfigs(paths: Array<string>): OwningTask<OwningArray<Config>>
  var tasks := new OwningArray<OwningTask<Config>>();
  for var path in paths do
    tasks.Add(LoadConfigAsync(path));    // each task is OwningTask<Config>; transfers in
  end;
  return await Task.WhenAllOwned(move tasks); // tasks consumed; returned array owns the configs
end LoadAllConfigs;
