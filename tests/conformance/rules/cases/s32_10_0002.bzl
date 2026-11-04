//! case: s32_10_0002
//! doc: spec
//! section: 32.10
//! title: an extern return value is received as the declared type
//! rule: The Bezel compiler guarantees: … The return value, if any, is treated as
//!   the declared type.
//! cites: 32.10, 32.6
//! expect: run
//! stdout: 5
//! notes: twin of s32_10_0001 (argument type) and s32_10_0003 (return type).
module Conf;

extern "libc"
  public method strlen(s: CString): ulong;
end;

public class Program
  public static method Main()
    var s := "hello";
    using var cs := CString.From(s);
    var n: ulong := strlen(cs);
    Console.WriteLine(n);
  end Main;
end Program;
