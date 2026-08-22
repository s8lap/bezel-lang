if pool.TryPopCached("primary", out using var c) then
  UseConnection(c);                // owned, and narrowed non-null by `when true`
end;                               // c disposed at the end of the enclosing block
