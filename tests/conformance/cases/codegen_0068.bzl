public method TryListen(rawPort: string): bool          // caller branches → a value shape
  var port := int.ParseOrNull(rawPort);
  if port = null then return false; end;
  Bind(port);                                            // narrowed by the early exit
  return true;
end TryListen;

public method Listen(rawPort: string)                   // boundary-consumed → a typed exception
  var port := int.ParseOrNull(rawPort);
  if port = null then throw new FormatException($"not a port: {rawPort}"); end;
  Bind(port);
end Listen;
