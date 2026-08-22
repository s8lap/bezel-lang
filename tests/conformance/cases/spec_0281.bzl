catch
  var ex: SqlException => throw new DataAccessException("query failed", ex),   // ex borrowed for the ctor call; snapshot taken, instance not retained (§31.4)
  var ex: Exception => Log(ex)
end;
