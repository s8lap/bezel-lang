//! case: s32_10_0006
//! doc: spec
//! section: 32.10
//! title: a boundary out recipient is definitely assigned after the extern call returns
//! rule: For a boundary `out` parameter (§32.5): the declared write actually
//!   occurring — after the call the recipient is *treated* as definitely assigned
//! cites: 32.10, 32.5
//! expect: run
//! output: 42
//!   True
//! notes: strtol stores the end pointer through its char** argument; reading the
//!   `out var` recipient after the call is admitted without any prior assignment.
module Conf;

extern "libc"
  public method strtol(s: CString, out endp: IntPtr, radix: int): long;
end;

public class Program
  public static method Main()
    using var cs := CString.From("42");
    var v := strtol(cs, out var endp, 10);
    Console.WriteLine(v);
    Console.WriteLine(endp <> IntPtr.Zero);
  end Main;
end Program;
