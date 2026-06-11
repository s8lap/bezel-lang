private owned _cache: OwnedDictionary<string, Connection>;

method TryGetCached(name: string, out owned conn: Connection?): bool
  if not _cache.ContainsKey(name) then
    conn := null;                  // every out path must assign
    return false;
  end;
  conn := _cache.PopValue(name);   // takes ownership out of the cache
  return true;
end TryGetCached;
