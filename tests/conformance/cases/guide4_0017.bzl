private static _cfg: Task<Config>? := null;              // §22.12 rule 2(ii)
public static async method GetConfig(): Task<Config>
  if _cfg = null then _cfg := LoadConfigAsync(); end;
  return await _cfg!;                                    // every caller borrows through the cache
end GetConfig;
