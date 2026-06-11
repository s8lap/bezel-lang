try
  DoWork();
catch
  var ex: TimeoutException => RetryWithBackoff(ex),
  var ex: Exception => do          // catch-all
    Log(ex);
    throw;
  end
end;
