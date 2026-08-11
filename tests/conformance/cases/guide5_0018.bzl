using var buf := new OwningBuffer<byte>(4096);   // runtime-sized, value slots
var header := buf.AsSpan(0, 64);                 // zero-copy writable view
header.Fill(0);                                  // bulk write through the view

var s := "Content-Type: text/plain";
var kind := s.AsSpan(14);                        // ReadOnlyStringSpan — no allocation
if kind.StartsWith("text/") then Accept(); end;
