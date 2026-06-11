try
  DoWork();
catch
  var ex: ArgumentException when ex.ParamName = "amount" => Console.WriteLine("Invalid amount"),
  InvalidOperationException => Console.WriteLine("Invalid state"),
  var ex: IOException, TimeoutException => Retry(ex),
  var ex: Exception => do
    Log(ex);
    throw;
  end
end;
