private static _cfg: Task<Config>? := null;
public static async method GetConfig(): Task<Config>
  if _cfg = null then _cfg := LoadConfigAsync(); end;
  return await _cfg!;
end GetConfig;
