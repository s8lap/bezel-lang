class ResponseWriter
  // Our own file-backed stream, owned for the lifetime of this writer.
  private owned _fallback: FileStream :=
      new FileStream("responses.log", FileMode.Append);

  // `dest` is a borrowed parameter (no `owned`) — the caller owns it and
  // guarantees it outlives this call. `_fallback` is a field — it lives as
  // long as this ResponseWriter. Both outlive the `if` below.
  public method Write(dest: Stream?, payload: Array<byte>)
    // Both `dest` and `_fallback` are borrows (this method owns neither),
    // so they share ownership and `??` is well-formed. `??` is rejected
    // only when its two sides disagree on ownership (see the note after
    // this section on why `dest ?? new FileStream(...)` does not compile).
    var target: Stream := dest ?? _fallback;   // borrows whichever is chosen
    target.Write(payload, 0, payload.Length);
    target.Flush();
  end Write;
end ResponseWriter;
