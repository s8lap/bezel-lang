public method Write(dest: Stream?, payload: Array<byte>)
  var target: Stream;
  if dest <> null then
    target := dest;
  else
    using var probe := new FileStream("tmp", FileMode.Create);
    target := probe;       // probe is disposed at this branch's end...
  end;
  target.Write(payload, 0, payload.Length);   // ...so target dangles here — ERROR
  target.Flush();
end Write;
