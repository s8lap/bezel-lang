private _lastErrorText: string?;

method Handle()
  try
    Risky();
  catch
    var ex: Exception => RecordError(ex)
  end;
end Handle;

private method RecordError(ex: Exception)
  _lastErrorText := ex.Message;            // string is fine to keep
  Log($"{ex}\n{ex.StackTrace}");           // the hole dispatches to Exception.ToString():
                                           //   qualified type name, message, both chains
end RecordError;
