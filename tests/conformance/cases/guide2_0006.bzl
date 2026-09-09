private owned _cache: OwningDictionary<string, Connection> := new OwningDictionary<string, Connection>();

method TryPopCached(name: string, out owned conn: Connection? when true): bool
  if not _cache.ContainsKey(name) then
    conn := null;                  // every out path must assign (throw paths exempt)
    return false;
  end;
  conn := _cache.PopValue(name);   // takes ownership out of the cache
  return true;
end TryPopCached;
