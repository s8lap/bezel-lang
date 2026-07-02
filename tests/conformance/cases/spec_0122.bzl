private owned _cache: OwningDictionary<string, Connection>;

public method TryGetCached(name: string, out owned conn: Connection?): bool
  if not _cache.ContainsKey(name) then
    conn := null;
    return false;
  end;
  conn := _cache.PopValue(name);    // takes ownership, removes from dictionary
  return true;
end TryGetCached;
