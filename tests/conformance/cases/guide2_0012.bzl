if pool.TryGetCached("primary", out using var c) then
  UseConnection(c!);               // bool guarantee makes ! safe; c is owned here
end;                               // c disposed at end of this block
