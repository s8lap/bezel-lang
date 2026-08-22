case ParsePort(raw) of
  Ok(var p)    => Use(p),
  Error(var m) => Use(BuildExpensiveMessage(m))     // computed only on failure
end;
var port := ParsePort(raw).UnwrapOr(8080);          // a constant fallback is fine eagerly
