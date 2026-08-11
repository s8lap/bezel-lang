public static class AppConfig                    // LoadConfigAsync(): Task<Config>
  private static _cfg: Task<Config>? := null;    // §22.12 rule 2(ii) task cache

  public static method Preload()                 // call from Main before any spawn
    _cfg := LoadConfigAsync();                   // plain sync assignment of the task
  end Preload;

  public static async method GetConfig(): Task<Config>   // pattern (b)'s accessor
    if _cfg = null then _cfg := LoadConfigAsync(); end;
    return await _cfg!;
  end GetConfig;
end AppConfig;

public static async method Main(): Task<int>
  AppConfig.Preload();                           // eager: assigned before any spawn
  return Run(await AppConfig.GetConfig());       // Run(cfg: Config): int — first use; fail-fast at startup
end Main;
