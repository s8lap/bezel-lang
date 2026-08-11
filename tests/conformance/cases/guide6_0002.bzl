// WRONG: rawPort arrives from outside the program. Its invalidity is an
// expected outcome, not a caller bug — no caller of Listen can guarantee it.
public method Listen(rawPort: string)
  requires(int.ParseOrNull(rawPort) <> null)   // misclassified: band 2 dressed as band 1
  Bind(int.ParseOrNull(rawPort)!);
end Listen;
