public method TryListen(rawPort: string): bool
  var port := int.ParseOrNull(rawPort);
  if port = null then
    return false;                 // expected outcome, reported as a value
  end;
  Bind(port);                     // narrowed by the early exit
  return true;
end TryListen;
