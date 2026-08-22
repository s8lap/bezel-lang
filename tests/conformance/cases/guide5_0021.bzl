using var scratch := new OwningBuffer<byte>(4096);       // one buffer for the whole loop
for var line in lines do
  var n := Encoding.UTF8.GetBytes(line, scratch.AsSpan());  // zero per-call allocation
  Send(scratch.AsReadOnlySpan(0, n));                       // hand out just the used range
end;
