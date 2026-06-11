public method Write(dest: Stream?, payload: Array<byte>)
  if dest <> null then
    Emit(dest, payload);                       // borrows the caller's stream
  else
    using var fallback := new FileStream("tmp", FileMode.Create);
    Emit(fallback, payload);                    // borrows the local fallback
  end;
end Write;

private method Emit(sink: Stream, payload: Array<byte>)
  sink.Write(payload, 0, payload.Length);
  sink.Flush();
end Emit;
