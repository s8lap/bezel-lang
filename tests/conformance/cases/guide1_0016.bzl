public method Write(dest: Stream?, payload: Array<byte>)
  if dest <> null then
    dest.Write(payload, 0, payload.Count);   // borrow; caller still owns dest
    dest.Flush();
  else
    using var fallback := new FileStream("tmp", FileMode.Create);
    fallback.Write(payload, 0, payload.Count);
    fallback.Flush();
  end;   // fallback disposed here, on the only path where it exists
end Write;
