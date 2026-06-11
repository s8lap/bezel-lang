try
  var data := Parse(input);
  Process(data);
catch
  var e: FormatException => Log($"bad input: {e.Message}"),
  var e: InvalidOperationException => Log($"bad state: {e.Message}")
finally
  Log("done");
end;
