method Header(): Span<byte>
  using var buf := new OwningBuffer<byte>(64);
  return buf.AsSpan();           // ERROR: the span borrows buf, disposed at scope end
end Header;
