public async method LoadAllConfigs(paths: Array<string>): OwnedTask<OwnedArray<Config>>
  var tasks := new OwnedArray<OwnedTask<Config>>();
  for var path in paths do
    tasks.Add(LoadConfigAsync(path));    // each task is OwnedTask<Config>; transfers in
  end;
  return await Task.WhenAllOwned(tasks); // tasks consumed; returned array owns the configs
end LoadAllConfigs;
