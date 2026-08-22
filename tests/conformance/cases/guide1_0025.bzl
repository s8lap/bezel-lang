public method Open(): owned Session
  var conn := Connect();
  using var g := new ScopeGuard(lambda() => conn.Rollback());
  Configure(conn);
  g.Dismiss();
  return conn;             // ERROR: conn is frozen while g is live (spec §28.14)
end Open;
