if pool.TryTakeCached("primary", out using var c) then
  // c is owned Connection? — narrowed non-null here by the `when true` postcondition
  UseConnection(c);
end;
