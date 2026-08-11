public method Listen(rawPort: string)
  var port := int.ParseOrNull(rawPort);
  if port = null then
    // Boundary-consumed expected failure: a typed exception, still never
    // a requires. FormatException is the built-in format fault (spec §31.4).
    throw new FormatException($"not a port: {rawPort}");
  end;
  Bind(port);                                     // narrowed by the early exit
end Listen;
