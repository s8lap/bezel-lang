public property [i: int]: double => case i
  0 => X,
  1 => Y,
  2 => Z,
  _ => throw new IndexOutOfRangeException()
end;

var name := user.Name if user <> null else
            throw new InvalidOperationException("user is null");

var config := LoadConfig() ?? throw new InvalidOperationException("no config");

try
  DoWork();
catch
  var ex: TransientException => Retry(ex),
  var ex: Exception => throw          // bare throw: re-raises the current exception
end;
