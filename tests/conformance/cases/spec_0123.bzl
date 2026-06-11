if pool.TryGetCached("primary", out using var c) then
  // c is owned Connection? here — but the bool guard tells us it's non-null
  UseConnection(c!);    // ! is fine; the API contract guarantees non-null on true return
end;
