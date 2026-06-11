private owned _lastError: Exception?;

method Handle()
  try
    Risky();
  catch
    var ex: Exception => _lastError := ex;   // ERROR: ex is a borrow;
  end;                                        //  cannot assign to an owned field
end Handle;
