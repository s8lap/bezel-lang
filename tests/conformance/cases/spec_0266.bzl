catch
  var ex: SqlException => throw new DataAccessException("query failed", ex),
  var ex: Exception => Log(ex)
end;
