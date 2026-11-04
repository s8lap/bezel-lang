//! case: s32_10_0001
//! doc: spec
//! section: 32.10
//! title: an extern argument must have the declared parameter type - a string is not a CString
//! rule: The Bezel compiler guarantees: … Arguments passed to an `extern` method are
//!   of the declared types.
//! cites: 32.10, 32.6
//! expect: reject
//! diagnostic: CString
//! notes: valid twin is s32_10_0002, which passes `CString.From(s)`.
module Conf;

extern "libc"
  public method strlen(s: CString): ulong;
end;

public class Program
  public static method Main()
    var s := "hello";
    var n: ulong := strlen(s);
    Console.WriteLine(n);
  end Main;
end Program;
