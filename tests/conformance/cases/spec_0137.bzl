if factory.TryCreate(config, out using var conn) then
  // conn is owned and non-null here; disposed at end of enclosing block
end;
