public method Wrong(replace: bool): owned FileStream
  var old := move _resource;
  if replace then _resource := new FileStream(_path, FileMode.Open); end;
  return old;                      // ERROR: the field is unrestored on the false path
end Wrong;

private owned _lastError: Exception?;
try Risky(); catch
  var ex: Exception => _lastError := ex;    // ERROR: `ex` is a borrow whose lifetime is the arm
end;

method Header(): Span<byte>
  using var buf := new OwningBuffer<byte>(64);
  return buf.AsSpan();             // ERROR: a borrow-bearing record may not outlive its buffer
end Header;
