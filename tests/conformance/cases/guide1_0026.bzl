public method Open(): owned Session
  var conn := Connect();
  do
    using var g := new ScopeGuard(lambda() => conn.Rollback());
    Configure(conn);
    g.Dismiss();
  end;                     // g's scope ends; the freeze on conn lifts
  return conn;
end Open;
