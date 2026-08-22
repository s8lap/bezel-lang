public method Listen(rawPort: string)
  requires(int.ParseOrNull(rawPort) <> null)     // WRONG: external input dressed as a caller contract

  Bind(int.ParseOrNull(rawPort)!);
end Listen;
