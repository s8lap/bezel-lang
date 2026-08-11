catch
  var ex: TransientException => Retry(ex),
  var ex: Exception => throw          // re-raise unrecognized exceptions
end;
