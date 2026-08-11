  public method Write(dest: Stream?, payload: Array<byte>)
    var target: Stream;
    if dest <> null then
      target := dest;          // borrows the caller's stream
    else
      target := _fallback;     // borrows our own field
    end;
    // Merge: `target` is a borrow on both branches, and both sources
    // (the caller's `dest`, or our `_fallback` field) outlive this point.
    target.Write(payload, 0, payload.Count);
    target.Flush();
  end Write;
