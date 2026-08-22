var displayName: string := user?.Name ?? "Anonymous";
// with `private owned _cache: Cache?;` — a fresh `new` requires an `owned` field (§28.6)
_cache ??= new Cache();             // assign only if currently null
