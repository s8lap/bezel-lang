//! case: s32_10_0003
//! doc: spec
//! section: 32.10
//! title: an extern return value of declared type ulong does not implicitly narrow to int
//! rule: The Bezel compiler guarantees: … The return value, if any, is treated as
//!   the declared type.
//! cites: 32.10, 5.1
//! expect: reject
//! diagnostic: ulong
//! notes: valid twin is s32_10_0002, which receives the result as `ulong`.
module Conf;

extern "libc"
  public method strlen(s: CString): ulong;
end;

public class Program
  public static method Main()
    var s := "hello";
    using var cs := CString.From(s);
    var n: int := strlen(cs);
    Console.WriteLine(n);
  end Main;
end Program;
