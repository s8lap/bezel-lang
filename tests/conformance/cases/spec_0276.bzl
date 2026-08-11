try
  DoWork();
catch
  var ex: TimeoutException => RetryWithBackoff(ex),
  var ex: Exception => do          // the catch-all (§31.2.3)
    Log(ex);
    throw;
  end
end;
